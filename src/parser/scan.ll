%{
// scan.ll — Placeholder Flex file.
//
// The actual SQL scanner is hand-written in scanner.cpp. This file exists
// only to satisfy the CMake FLEX_TARGET requirement in src/CMakeLists.txt.
// The generated lex.yy.cpp is compiled but never called; the Bison parser
// invokes the hand-written yylex() defined in scanner.cpp instead.
%}

%%
.  { /* no-op: discard one character */ }
%%
