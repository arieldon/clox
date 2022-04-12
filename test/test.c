#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PATH_LENGTH 256
#define MAX_EXPECTATIONS_PER_TEST 8

static char *interpreter_path;
static char *tests_path;

typedef struct Test {
    struct Test *next;
    char *expectations[MAX_EXPECTATIONS_PER_TEST];
    char path[MAX_PATH_LENGTH];
} Test;

static void
append_tests(Test **tests, const char *tests_path)
{
    static bool recursed = false;

    DIR *tests_dir;
    if ((tests_dir = opendir(tests_path)) == NULL) {
        perror("opendir");
        fprintf(stderr, "error: failed to open path '%s'\n", tests_path);
        exit(EXIT_FAILURE);
    }

    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(tests_dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        if (entry->d_type == DT_DIR) {
            recursed = true;

            // Prepend name of current directory to filename.
            size_t dirlen = strlen(tests_path);
            size_t namelen = strlen(entry->d_name);
            assert(dirlen + namelen + 1 <= 256);

            memcpy(entry->d_name + dirlen, entry->d_name, namelen);
            memcpy(entry->d_name, tests_path, dirlen);
            entry->d_name[dirlen + namelen] = 0;

            append_tests(tests, entry->d_name);
            recursed = false;
        }

        if (entry->d_type == DT_REG && strstr(entry->d_name, ".lox") != NULL) {
            Test *test = calloc(1, sizeof(Test));
            if (test == NULL) {
                fprintf(stderr, "error: failed to allocate memory for test '%s'\n", entry->d_name);
                goto exit;
            }
            for (size_t i = 0; i < MAX_EXPECTATIONS_PER_TEST; ++i) test->expectations[i] = NULL;

            if (recursed) {
                size_t parentlen = strlen(tests_path);
                size_t namelen = strlen(entry->d_name);
                assert(parentlen + namelen + 1 <= MAX_PATH_LENGTH);

                memcpy(test->path, tests_path, parentlen);
                test->path[parentlen] = '/';
                memcpy(test->path + parentlen + 1, entry->d_name, namelen);
            } else {
                memcpy(test->path, entry->d_name, strlen(entry->d_name));
            }

            test->next = *tests;
            *tests = test;
        }
    }

    if (errno) {
        perror("readdir");
        fprintf(stderr, "error: failed to read a directory entry in directory '%s'\n", tests_path);
        errno = 0;
    }

exit:
    closedir(tests_dir);
}

static void
parse_test(Test *test)
{
    FILE *source = fopen(test->path, "r");
    if (source == NULL) {
        perror("fopen");
        fprintf(stderr, "error: failed to open file '%s'\n", test->path);
        return;
    }

    fseek(source, 0, SEEK_END);
    size_t file_size = ftell(source);
    rewind(source);

    char *buffer = calloc(file_size + 1, sizeof(char));
    if (buffer == NULL) {
        perror("calloc");
        fprintf(stderr, "error: failed to allocate buffer for contents of file '%s'\n", test->path);
        goto free_source;
    }
    buffer[file_size] = '\0';

    size_t bytes_read = fread(buffer, sizeof(char), file_size, source);
    if (bytes_read != file_size) {
        perror("fread");
        fprintf(stderr, "error: failed to read file '%s'\n", test->path);
        goto free_buffer;
    }

    char *eol, *expectation = buffer;
    for (size_t i = 0; i < MAX_EXPECTATIONS_PER_TEST; ++i) {
        expectation = strstr(expectation, "expect: ");
        if (expectation == NULL) break;
        eol = strchr(expectation, '\n');

        // 8 represents the length of "expect: ".
        size_t length = eol - expectation - 8;
        test->expectations[i] = calloc(length + 1, sizeof(char));
        test->expectations[i][length] = 0;
        memcpy(test->expectations[i], expectation + 8, length);

        expectation = eol;
    }

free_buffer:
    free(buffer);
free_source:
    fclose(source);
}

static bool
run_test(Test *test)
{
    // TODO Test if reroute using a pipe to a buffer faster than temporary file.

    bool ret = true;
    printf("run_test: %s\n", test->path);

    parse_test(test);

    const char *tmp_path = "./clox_test_tmp";
    FILE *tmp = fopen(tmp_path, "w+");
    if (tmp == NULL) {
        perror("fopen");
        fprintf(stderr, "error: failed to create temporary file '%s' for test '%s'\n",
                tmp_path, test->path);
        return false;
    }

    // Reroute stdout to temporary file.
    if (freopen(tmp_path, "w", stdout) == NULL) {
        perror("freopen");
        fprintf(stderr, "error: failed to reroute stdout to temporary file '%s' for test '%s'\n",
                tmp_path, test->path);
        ret = false;
        goto free_tmp;
    }

    // Run clox in a child process.
    pid_t child = fork();
    if (child == -1) {
        fprintf(stderr, "error: failed to spawn child process for interpreter\n");
        ret = false;
        goto restore_stdout;
    } else if (child == 0) {
        char *arguments[] = { "clox", test->path, (char *)0 };
        if (execv(interpreter_path, arguments) == -1) {
            perror("execv");
            fprintf(stderr, "error: child process failed to load interpreter at path '%s'\n",
                    interpreter_path);
            exit(EXIT_FAILURE);
        }
    } else {
        int wstatus;
        if (wait(&wstatus) == -1) {
            perror("wait");
            fprintf(stderr, "error: parent failed to wait for child process\n");
            ret = false;
            goto restore_stdout;
        } else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == EXIT_FAILURE) {
            fprintf(stderr, "error: child process failed\n");
            ret = false;
            goto restore_stdout;
        }
    }

    char buffer[64] = {0};
    for (int i = 0; i < MAX_EXPECTATIONS_PER_TEST; ++i) {
        if (test->expectations[i] == NULL) break;

        if (fgets(buffer, 64, tmp) == NULL) {
            perror("fgets");
            fprintf(stderr, "unable to read a line from temporary file '%s' for test '%s'\n",
                    tmp_path, test->path);
        }

        size_t linelen_from_file = strlen(buffer);
        size_t linelen_from_expect = strlen(test->expectations[i]);
        size_t linelen = linelen_from_file > linelen_from_expect ?
            linelen_from_expect : linelen_from_file;
        if (memcmp(buffer, test->expectations[i], linelen) != 0) {
            fprintf(stderr, "Expectation: %s; Reality: %s", test->expectations[i], buffer);
            return false;
        }
    }

    if (errno) {
        perror("fgets");
        fprintf(stderr,
                "error: failed to read line from temporary file '%s' created for test '%s'\n",
                tmp_path, test->path);
        ret = false;
    }

restore_stdout:
    if (freopen("/dev/tty", "w", stdout) == NULL) {
        perror("freopen");
        fprintf(stderr, "error: failed to restore stdout after temporarily rerouting it to file '%s' for test '%s'\n",
                tmp_path, test->path);
        exit(EXIT_FAILURE);
    }

free_tmp:
    if (fclose(tmp) == EOF) {
        perror("fclose");
        fprintf(stderr, "error: failed to close temporary file '%s' created for test '%s'",
                tmp_path, test->path);
    } else if (remove(tmp_path) == -1) {
        perror("remove");
        fprintf(stderr, "error: failed to remove temporary file '%s' created for test '%s'\n",
                tmp_path, test->path);
    }

    return ret;
}

int
main(int argc, char *argv[])
{
    // Sanitize input.
    char *program_name = argv[0];

    if (argc != 3) {
        fprintf(stdout, "usage: %s interpreter tests\n", program_name);
        exit(EXIT_FAILURE);
    }

    interpreter_path = argv[1];
    tests_path = argv[2];


    // Create linked list of Lox scripts for testing by recursively searching
    // given directory for *.lox files.
    Test *tests = NULL;
    append_tests(&tests, tests_path);


    // Run each test, outputting results only for failed tests.
    int total_tests = 0;
    int tests_passed = 0;

    Test *test = tests;
    while (test) {
        tests_passed += run_test(test);
        test = test->next;
        ++total_tests;
    }

    // Output final results.
    printf("%d of %d tests passed.\n", tests_passed, total_tests);
    return 0;
}
