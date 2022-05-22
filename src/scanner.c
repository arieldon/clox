#include <stdio.h>
#include <string.h>

#include "common.h"
#include "scanner.h"

// As with the VM, create a single instance of Scanner to manage state and
// allow all modules access.
Scanner scanner;

void
initScanner(const char *source)
{
    scanner.start = source;
    scanner.current = source;
    scanner.line = 1;
}

static bool
isAlpha(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
            c == '_';
}

static bool
isDigit(char c)
{
    return c >= '0' && c <= '9';
}

static bool
isAtEnd(void)
{
    return *scanner.current == '\0';
}

static char
advance(void)
{
    ++scanner.current;
    return scanner.current[-1];
}

static char
peek(void)
{
    // Return current character without consuming it.
    return *scanner.current;
}

static char
peekNext(void)
{
    if (isAtEnd()) return '\0';
    return scanner.current[1];
}

static bool
match(char expected)
{
    if (isAtEnd()) return false;
    if (*scanner.current != expected) return false;
    ++scanner.current;
    return true;
}

static Token
makeToken(TokenType type)
{
    return (Token){
        .type = type,
        .start = scanner.start,
        .length = (int)(scanner.current - scanner.start),
        .line = scanner.line,
    };
}

static Token
errorToken(const char *message)
{
    return (Token){
        .type = TOKEN_ERROR,
        .start = message,
        .length = (int)strlen(message),
        .line = scanner.line,
    };
}

static void
skipCharacters(void)
{
    for (;;) {
        switch (peek()) {
            case ' ':
            case '\r':
            case '\t':
                advance();
                break;
            case '\n':
                ++scanner.line;
                advance();
                break;
            case '/':
                if (peekNext() == '/') {
                    while (peek() != '\n' && !isAtEnd()) advance();
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static TokenType
checkKeyword(int start, int length, const char *rest, TokenType type)
{
    if (scanner.current - scanner.start == start + length &&
            memcmp(scanner.start + start, rest, length) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static TokenType
identifierType(void)
{
    // Use ad hoc trie to parse keywords from variable names.
    switch (scanner.start[0]) {
        case 'a': return checkKeyword(1, 2, "nd", TOKEN_AND);
        case 'b': return checkKeyword(1, 4, "reak", TOKEN_BREAK);
        case 'c':
            if (scanner.current - scanner.start > 1) {
                switch (scanner.start[1]) {
                    case 'l': return checkKeyword(2, 3, "ass", TOKEN_CLASS);
                    case 'o': return checkKeyword(2, 6, "ntinue", TOKEN_CONTINUE);
                }
            }
            break;
        case 'e': return checkKeyword(1, 3, "lse", TOKEN_ELSE);
        case 'f':
            if (scanner.current - scanner.start > 1) {
                switch (scanner.start[1]) {
                    case 'a': return checkKeyword(2, 3, "lse", TOKEN_FALSE);
                    case 'o': return checkKeyword(2, 1, "r", TOKEN_FOR);
                    case 'u': return checkKeyword(2, 1, "n", TOKEN_FUN);
                }
            }
            break;
        case 'i': return checkKeyword(1, 1, "f", TOKEN_IF);
        case 'n': return checkKeyword(1, 2, "il", TOKEN_NIL);
        case 'o': return checkKeyword(1, 1, "r", TOKEN_OR);
        case 'p': return checkKeyword(1, 4, "rint", TOKEN_PRINT);
        case 'r': return checkKeyword(1, 5, "eturn", TOKEN_RETURN);
        case 's': return checkKeyword(1, 4, "uper", TOKEN_SUPER);
        case 't':
            if (scanner.current - scanner.start > 1) {
                switch (scanner.start[1]) {
                    case 'h': return checkKeyword(2, 2, "is", TOKEN_THIS);
                    case 'r': return checkKeyword(2, 2, "ue", TOKEN_TRUE);
                }
            }
            break;
        case 'v': return checkKeyword(1, 2, "ar", TOKEN_VAR);
        case 'w': return checkKeyword(1, 4, "hile", TOKEN_WHILE);
    }
    return TOKEN_IDENTIFIER;
}

static Token
identifier(void)
{
    while (isAlpha(peek()) || isDigit(peek())) advance();
    return makeToken(identifierType());
}

static Token
number(void)
{
    while (isDigit(peek())) advance();

    // Account for decimal point if it exists.
    if (peek() == '.' && isDigit(peekNext())) {
        // Consume point and continue to end of number.
        advance();
        while (isDigit(peek())) advance();
    }

    return makeToken(TOKEN_NUMBER);
}

static Token
string(void)
{
    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n') ++scanner.line;
        advance();
    }

    if (isAtEnd()) return errorToken("unterminated string");

    advance();
    return makeToken(TOKEN_STRING);
}

Token
scanToken(void)
{
    // Skip whitespace and comments.
    skipCharacters();

    // Each call to scanToken() produces a complete token, so the current
    // position represents the start of a new token.
    scanner.start = scanner.current;

    if (isAtEnd()) return makeToken(TOKEN_EOF);

    char c = advance();
    if (isAlpha(c)) return identifier();
    if (isDigit(c)) return number();

    switch (c) {
        case '(': return makeToken(TOKEN_LEFT_PAREN);
        case ')': return makeToken(TOKEN_RIGHT_PAREN);
        case '{': return makeToken(TOKEN_LEFT_BRACE);
        case '}': return makeToken(TOKEN_RIGHT_BRACE);
        case ';': return makeToken(TOKEN_SEMICOLON);
        case ',': return makeToken(TOKEN_COMMA);
        case '.': return makeToken(TOKEN_DOT);
        case '-': return makeToken(TOKEN_MINUS);
        case '+': return makeToken(TOKEN_PLUS);
        case '%': return makeToken(TOKEN_PERCENT);
        case '/': return makeToken(TOKEN_SLASH);
        case '*': return makeToken(TOKEN_STAR);
        case '?': return makeToken(TOKEN_QMARK);
        case ':': return makeToken(TOKEN_COLON);
        case '!': return makeToken(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
        case '=': return makeToken(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '<': return makeToken(match('=') ? TOKEN_LESSER_EQUAL : TOKEN_LESSER);
        case '>': return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        case '"': return string();
    }

    return errorToken("unexpected character");
}
