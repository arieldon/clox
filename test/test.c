#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PATH_LENGTH 256
#define MAX_EXPECTATIONS_PER_TEST 16
#define MAX_EXPECTATION_LENGTH 48
#define MAX_BUFFER_LENGTH 512

#define EXPECTATION_STR "expect: "
#define EXPECTATION_STR_LENGTH 8

static char buffer[MAX_BUFFER_LENGTH] = {0};
static char *interpreter_path = NULL;
static char *tests_path = NULL;

typedef struct Test {
    struct Test *next;
    char path[MAX_PATH_LENGTH];
    char expectations[MAX_EXPECTATIONS_PER_TEST][MAX_EXPECTATION_LENGTH];
} Test;

void
logerr(char *fmt, ...)
{
    // Assume this functions runs directly after previous failed function. If
    // this assumption holds, it's safe to copy `errno` and call the following
    // IO functions.
    int errcode = errno;

    va_list ap;
    va_start(ap, fmt);

    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    if (errcode) {
        fprintf(stderr, " (%s)\n", strerror(errcode));
    } else {
        fprintf(stderr, "\n");
    }

    va_end(ap);
}

static void
append_tests(Test **tests, const char *tests_path)
{
    static bool recursed = false;

    DIR *tests_dir;
    if ((tests_dir = opendir(tests_path)) == NULL) {
        logerr("failed to open path '%s'", tests_path);
        exit(EXIT_FAILURE);
    }

    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(tests_dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        if (!recursed) {
            size_t dirlen = strlen(tests_path);
            size_t namelen = strlen(entry->d_name);
            assert(dirlen + namelen + 1 <= 256);

            memcpy(entry->d_name + dirlen, entry->d_name, namelen);
            memcpy(entry->d_name, tests_path, dirlen);
            entry->d_name[dirlen + namelen] = 0;
        }

        // Only support one layer of recursion.
        if (!recursed && entry->d_type == DT_DIR) {
            recursed = true;
            append_tests(tests, entry->d_name);
            recursed = false;
        }
        if (recursed && entry->d_type == DT_DIR) {
            logerr("ignore nested directory '%s'", entry->d_name);
        }

        if (entry->d_type == DT_REG && strstr(entry->d_name, ".lox") != NULL) {
            Test *test = calloc(1, sizeof(Test));
            if (test == NULL) {
                logerr("failed to allocate memory for test '%s'", entry->d_name);
                goto exit;
            }

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

    // Check errno for readdir().
    if (errno) {
        logerr("failed to read a directory entry in directory '%s'", tests_path);
        errno = 0;
    }

exit:
    closedir(tests_dir);
}

static bool
parse_test(Test *test)
{
    bool ret = true;

    int sourcefd = open(test->path, O_RDONLY);
    if (sourcefd == -1) {
        logerr("failed to open file '%s'", test->path);
        return false;
    }

    struct stat sb = {0};
    if (fstat(sourcefd, &sb) == -1) {
        logerr("failed to obtain information about file '%s'", test->path);
        ret = false;
        goto free_source;
    }

    char *file_buffer = calloc(sb.st_size + 1, sizeof(char));
    if (file_buffer == NULL) {
        logerr("failed to allocate file_buffer for contents of file '%s'", test->path);
        ret = false;
        goto free_source;
    }
    file_buffer[sb.st_size] = '\0';

    ssize_t read_bytes = read(sourcefd, file_buffer, sb.st_size);
    if (read_bytes == -1) {
        logerr("failed to read file '%s'", test->path);
        goto free_file_buffer;
    }

    char *eol = NULL;
    char *expectation = file_buffer;
    for (size_t i = 0; i < MAX_EXPECTATIONS_PER_TEST; ++i) {
        expectation = strstr(expectation, "expect: ");
        if (expectation == NULL) break;
        eol = strchr(expectation, '\n');

        // Subtract 8 to remove "expect: " from string.
        size_t length = eol - expectation - 8;
        assert(length < MAX_EXPECTATION_LENGTH);
        memcpy(test->expectations[i], expectation + 8, length);

        expectation = eol;
    }

    if (test->expectations[MAX_EXPECTATIONS_PER_TEST - 1] != NULL && expectation != NULL) {
        logerr("too many tests, only %d are permitted per file", MAX_EXPECTATIONS_PER_TEST);
    }

free_file_buffer:
    free(file_buffer);
free_source:
    close(sourcefd);
    return ret;
}

static bool
run_test(Test *test)
{
    int fds[2];
    bool ret = true;

    printf("run_test: %s\n", test->path);
    if (!parse_test(test)) {
        logerr("failed to parse test '%s'", test->path);
        return false;
    }

    if (pipe(fds) == -1) {
        logerr("failed to create pipe for test '%s' to read and write between processes",
                test->path);
        return false;
    }

    int readfd = fds[0];
    int writefd = fds[1];

    // Run clox in a child process.
    switch (fork()) {
        case -1:
            logerr("failed to spawn child process to interpret test '%s'", test->path);
            ret = false;
            goto exit;
        case 0:
            // Child does not read.
            close(readfd);

            // Route stdout and stderr to pipe.
            dup2(writefd, STDOUT_FILENO);
            dup2(writefd, STDERR_FILENO);

            // Start interpreter.
            char *arguments[] = { "clox", test->path, NULL };
            if (execv(interpreter_path, arguments) == -1) {
                logerr("child process failed to load interpreter at path '%s'",
                        interpreter_path);
                exit(EXIT_FAILURE);
            }
        default:
            // Parent does not write.
            close(writefd);

            // Wait for child.
            int wstatus;
            if (wait(&wstatus) == -1) {
                logerr("parent failed to wait for child process");
                ret = false;
                goto exit;
            } else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == EXIT_FAILURE) {
                // Child process outputs error to stderr upon failure.
                logerr("child process returned an unexpected exit code %d", WEXITSTATUS(wstatus));
                ret = false;
                goto exit;
            };
    }

    memset(buffer, '\0', MAX_BUFFER_LENGTH);
    ssize_t read_bytes = read(readfd, buffer, MAX_BUFFER_LENGTH);
    if (read_bytes == -1) {
        logerr("failed to read buffer for test '%s'", test->path);
        ret = false;
        goto exit;
    }

    char *eol = NULL;
    char *actual = buffer;
    for (size_t i = 0; i < MAX_EXPECTATIONS_PER_TEST; ++i) {
        if (test->expectations[i][0] == 0) break;
        if ((eol = strchr(actual, '\n')) == NULL) break;
        *eol = '\0';

        size_t expectation_length = strlen(test->expectations[i]);
        size_t actual_length = strlen(actual);
        size_t length = expectation_length < actual_length ?
            expectation_length : actual_length;
        if (memcmp(test->expectations[i], actual, length) != 0) {
            fprintf(stderr, "\t(failure) expectation: %s | actual: %s\n",
                    test->expectations[i], actual);
            ret = false;
            goto exit;
        }

        actual = eol + 1;
        assert(actual < buffer + MAX_BUFFER_LENGTH);
    }

exit:
    close(readfd);
    return ret;
}

int
main(int argc, char *argv[])
{
    // Sanitize input.
    char *program_name = argv[0];

    if (argc != 3) {
        printf("usage: %s interpreter tests\n", program_name);
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

    Test *prev_test = NULL;
    Test *test = tests;
    while (test) {
        prev_test = test;
        tests_passed += run_test(test);
        test = test->next;
        ++total_tests;
        free(prev_test);
    }


    // Output final results.
    printf("%d of %d tests passed.\n", tests_passed, total_tests);
    return 0;
}
