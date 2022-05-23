# Clox

Clox is bytecode compiler and virtual machine interpreter for Lox from Part III
of Robert Nystrom's *Crafting Interpreters*.

Along with the features implemented and explained by Nystrom, this
implementation also supports:

- a run-length encoding format to map bytecode instructions to line numbers;
- `?:` C-style ternary or conditional expressions;
- `continue` and `break` statements to augment flow control in loops;
- `%` modulo arithmetic;
- and more.

That being said, Clox is still a toy. For example, it limits any scope to 256
variables, and while these hard limits are possible to fix, I'm not willing to
test the changes since Lox itself remains a toy to me, albeit a toy that taught
me a lot.


## Architecture

Clox contains three phases: scanner, compiler, and VM. Tokens stream from
scanner to compiler, and chunks of bytecode stream from compiler to VM.

### Scanner

The scanner outputs tokens given Lox source code from a file or the REPL.

Clox's scanner is lazy. It doesn't scan the next token until prompted by the
compiler because Lox grammar requires only a single token lookahead, and the
compiler demands two tokens at a time at most. Upon demand, the scanner
allocates tokens on the stack and passes their addresses to the compiler.

### Compiler

Fed a stream of tokens, the compiler parses these tokens and generates bytecode
in a single pass. It also reports static errors.

Clox's compiler first parses the tokens passed by the scanner. It first parses
prefix expressions, consuming the necessary tokens. The first token in an
expression must correspond to a prefix rule. The compiler throws an error,
outputs a message, and terminates otherwise. Then, the compiler attempts to
parse an infix expression if it exists and precedence allows.

This technique was first introduced by Vaughn Pratt,
[here](https://dl.acm.org/doi/10.1145/512927.512931), and Nystrom explains its
implementation it in the book.

After parsing a subset of tokens, the compiler emits analogous bytecode and
stores any constants in the expression for use at runtime.

### VM

The stack-based VM executes chunks of bytecode instructions. It also reports
runtime errors.


## Usage

```shell
$ make
```

Now, an executable `./build/clox` exists.

```shell
$ ./build/clox [script]
```

Optionally, compile with `make MODE=debug` to for better GDB support.

To install Clox globally, use `make install`. Note, it requires elevated
privileges.

```shell
$ clox [script]
```


## Test

While included tests are by no means comprehensive, run `make test` to test
Clox using Lox scripts in directory `test/`. Ensure compilation occurs with all
debug macros (`DEBUG_PRINT_CODE`, `DEBUG_TRACE_EXECUTION`, `DEBUG_STRESS_GC`,
`DEBUG_LOG_GC`) defined in `include/common.h` disabled. The testing program
uses the standard output and standard error streams to compare expected output
with actual output of the sample Lox programs. The tests encode expected output
with comments in the source code. The macros must be disabled because they
enable additional code that pollutes standard out.

```shell
$ make test
```
