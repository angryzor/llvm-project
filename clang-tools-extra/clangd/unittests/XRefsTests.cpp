//===-- XRefsTests.cpp  ---------------------------*- C++ -*--------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Annotations.h"
#include "Compiler.h"
#include "Matchers.h"
#include "ParsedAST.h"
#include "Protocol.h"
#include "SourceCode.h"
#include "SyncAPI.h"
#include "TestFS.h"
#include "TestIndex.h"
#include "TestTU.h"
#include "XRefs.h"
#include "index/FileIndex.h"
#include "index/MemIndex.h"
#include "index/SymbolCollector.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Index/IndexingAction.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ScopedPrinter.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <string>
#include <vector>

namespace clang {
namespace clangd {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Matcher;
using ::testing::UnorderedElementsAreArray;

class IgnoreDiagnostics : public DiagnosticsConsumer {
  void onDiagnosticsReady(PathRef File,
                          std::vector<Diag> Diagnostics) override {}
};

MATCHER_P2(FileRange, File, Range, "") {
  return Location{URIForFile::canonicalize(File, testRoot()), Range} == arg;
}

// Extracts ranges from an annotated example, and constructs a matcher for a
// highlight set. Ranges should be named $read/$write as appropriate.
Matcher<const std::vector<DocumentHighlight> &>
HighlightsFrom(const Annotations &Test) {
  std::vector<DocumentHighlight> Expected;
  auto Add = [&](const Range &R, DocumentHighlightKind K) {
    Expected.emplace_back();
    Expected.back().range = R;
    Expected.back().kind = K;
  };
  for (const auto &Range : Test.ranges())
    Add(Range, DocumentHighlightKind::Text);
  for (const auto &Range : Test.ranges("read"))
    Add(Range, DocumentHighlightKind::Read);
  for (const auto &Range : Test.ranges("write"))
    Add(Range, DocumentHighlightKind::Write);
  return UnorderedElementsAreArray(Expected);
}

TEST(HighlightsTest, All) {
  const char *Tests[] = {
      R"cpp(// Local variable
        int main() {
          int [[bonjour]];
          $write[[^bonjour]] = 2;
          int test1 = $read[[bonjour]];
        }
      )cpp",

      R"cpp(// Struct
        namespace ns1 {
        struct [[MyClass]] {
          static void foo([[MyClass]]*) {}
        };
        } // namespace ns1
        int main() {
          ns1::[[My^Class]]* Params;
        }
      )cpp",

      R"cpp(// Function
        int [[^foo]](int) {}
        int main() {
          [[foo]]([[foo]](42));
          auto *X = &[[foo]];
        }
      )cpp",

      R"cpp(// Function parameter in decl
        void foo(int [[^bar]]);
      )cpp",
  };
  for (const char *Test : Tests) {
    Annotations T(Test);
    auto AST = TestTU::withCode(T.code()).build();
    EXPECT_THAT(findDocumentHighlights(AST, T.point()), HighlightsFrom(T))
        << Test;
  }
}

MATCHER_P3(Sym, Name, Decl, DefOrNone, "") {
  llvm::Optional<Range> Def = DefOrNone;
  if (Name != arg.Name) {
    *result_listener << "Name is " << arg.Name;
    return false;
  }
  if (Decl != arg.PreferredDeclaration.range) {
    *result_listener << "Declaration is "
                     << llvm::to_string(arg.PreferredDeclaration);
    return false;
  }
  if (Def && !arg.Definition) {
    *result_listener << "Has no definition";
    return false;
  }
  if (Def && arg.Definition->range != *Def) {
    *result_listener << "Definition is " << llvm::to_string(arg.Definition);
    return false;
  }
  return true;
}
::testing::Matcher<LocatedSymbol> Sym(std::string Name, Range Decl) {
  return Sym(Name, Decl, llvm::None);
}
MATCHER_P(Sym, Name, "") { return arg.Name == Name; }

MATCHER_P(RangeIs, R, "") { return arg.range == R; }

TEST(LocateSymbol, WithIndex) {
  Annotations SymbolHeader(R"cpp(
        class $forward[[Forward]];
        class $foo[[Foo]] {};

        void $f1[[f1]]();

        inline void $f2[[f2]]() {}
      )cpp");
  Annotations SymbolCpp(R"cpp(
      class $forward[[forward]] {};
      void  $f1[[f1]]() {}
    )cpp");

  TestTU TU;
  TU.Code = SymbolCpp.code();
  TU.HeaderCode = SymbolHeader.code();
  auto Index = TU.index();
  auto LocateWithIndex = [&Index](const Annotations &Main) {
    auto AST = TestTU::withCode(Main.code()).build();
    return clangd::locateSymbolAt(AST, Main.point(), Index.get());
  };

  Annotations Test(R"cpp(// only declaration in AST.
        void [[f1]]();
        int main() {
          ^f1();
        }
      )cpp");
  EXPECT_THAT(LocateWithIndex(Test),
              ElementsAre(Sym("f1", Test.range(), SymbolCpp.range("f1"))));

  Test = Annotations(R"cpp(// definition in AST.
        void [[f1]]() {}
        int main() {
          ^f1();
        }
      )cpp");
  EXPECT_THAT(LocateWithIndex(Test),
              ElementsAre(Sym("f1", SymbolHeader.range("f1"), Test.range())));

  Test = Annotations(R"cpp(// forward declaration in AST.
        class [[Foo]];
        F^oo* create();
      )cpp");
  EXPECT_THAT(LocateWithIndex(Test),
              ElementsAre(Sym("Foo", Test.range(), SymbolHeader.range("foo"))));

  Test = Annotations(R"cpp(// defintion in AST.
        class [[Forward]] {};
        F^orward create();
      )cpp");
  EXPECT_THAT(
      LocateWithIndex(Test),
      ElementsAre(Sym("Forward", SymbolHeader.range("forward"), Test.range())));
}

TEST(LocateSymbol, WithIndexPreferredLocation) {
  Annotations SymbolHeader(R"cpp(
        class $p[[Proto]] {};
        void $f[[func]]() {};
      )cpp");
  TestTU TU;
  TU.HeaderCode = SymbolHeader.code();
  TU.HeaderFilename = "x.proto"; // Prefer locations in codegen files.
  auto Index = TU.index();

  Annotations Test(R"cpp(// only declaration in AST.
        // Shift to make range different.
        class Proto;
        void func() {}
        P$p^roto* create() {
          fu$f^nc();
          return nullptr;
        }
      )cpp");

  auto AST = TestTU::withCode(Test.code()).build();
  {
    auto Locs = clangd::locateSymbolAt(AST, Test.point("p"), Index.get());
    auto CodeGenLoc = SymbolHeader.range("p");
    EXPECT_THAT(Locs, ElementsAre(Sym("Proto", CodeGenLoc, CodeGenLoc)));
  }
  {
    auto Locs = clangd::locateSymbolAt(AST, Test.point("f"), Index.get());
    auto CodeGenLoc = SymbolHeader.range("f");
    EXPECT_THAT(Locs, ElementsAre(Sym("func", CodeGenLoc, CodeGenLoc)));
  }
}

TEST(LocateSymbol, All) {
  // Ranges in tests:
  //   $decl is the declaration location (if absent, no symbol is located)
  //   $def is the definition location (if absent, symbol has no definition)
  //   unnamed range becomes both $decl and $def.
  const char *Tests[] = {
      R"cpp(// Local variable
        int main() {
          int [[bonjour]];
          ^bonjour = 2;
          int test1 = bonjour;
        }
      )cpp",

      R"cpp(// Struct
        namespace ns1 {
        struct [[MyClass]] {};
        } // namespace ns1
        int main() {
          ns1::My^Class* Params;
        }
      )cpp",

      R"cpp(// Function definition via pointer
        int [[foo]](int) {}
        int main() {
          auto *X = &^foo;
        }
      )cpp",

      R"cpp(// Function declaration via call
        int $decl[[foo]](int);
        int main() {
          return ^foo(42);
        }
      )cpp",

      R"cpp(// Field
        struct Foo { int [[x]]; };
        int main() {
          Foo bar;
          bar.^x;
        }
      )cpp",

      R"cpp(// Field, member initializer
        struct Foo {
          int [[x]];
          Foo() : ^x(0) {}
        };
      )cpp",

      R"cpp(// Field, GNU old-style field designator
        struct Foo { int [[x]]; };
        int main() {
          Foo bar = { ^x : 1 };
        }
      )cpp",

      R"cpp(// Field, field designator
        struct Foo { int [[x]]; };
        int main() {
          Foo bar = { .^x = 2 };
        }
      )cpp",

      R"cpp(// Method call
        struct Foo { int $decl[[x]](); };
        int main() {
          Foo bar;
          bar.^x();
        }
      )cpp",

      R"cpp(// Typedef
        typedef int $decl[[Foo]];
        int main() {
          ^Foo bar;
        }
      )cpp",

      R"cpp(// Template type parameter
        template <typename [[T]]>
        void foo() { ^T t; }
      )cpp",

      R"cpp(// Template template type parameter
        template <template<typename> class [[T]]>
        void foo() { ^T<int> t; }
      )cpp",

      R"cpp(// Namespace
        namespace $decl[[ns]] {
        struct Foo { static void bar(); }
        } // namespace ns
        int main() { ^ns::Foo::bar(); }
      )cpp",

      R"cpp(// Macro
        #define MACRO 0
        #define [[MACRO]] 1
        int main() { return ^MACRO; }
        #define MACRO 2
        #undef macro
      )cpp",

      R"cpp(// Macro
       class TTT { public: int a; };
       #define [[FF]](S) if (int b = S.a) {}
       void f() {
         TTT t;
         F^F(t);
       }
      )cpp",

      R"cpp(// Macro argument
       int [[i]];
       #define ADDRESSOF(X) &X;
       int *j = ADDRESSOF(^i);
      )cpp",

      R"cpp(// Symbol concatenated inside macro (not supported)
       int *pi;
       #define POINTER(X) p # X;
       int i = *POINTER(^i);
      )cpp",

      R"cpp(// Forward class declaration
        class Foo;
        class [[Foo]] {};
        F^oo* foo();
      )cpp",

      R"cpp(// Function declaration
        void foo();
        void g() { f^oo(); }
        void [[foo]]() {}
      )cpp",

      R"cpp(
        #define FF(name) class name##_Test {};
        [[FF]](my);
        void f() { my^_Test a; }
      )cpp",

      R"cpp(
         #define FF() class [[Test]] {};
         FF();
         void f() { T^est a; }
      )cpp",

      R"cpp(// explicit template specialization
        template <typename T>
        struct Foo { void bar() {} };

        template <>
        struct [[Foo]]<int> { void bar() {} };

        void foo() {
          Foo<char> abc;
          Fo^o<int> b;
        }
      )cpp",

      R"cpp(// implicit template specialization
        template <typename T>
        struct [[Foo]] { void bar() {} };
        template <>
        struct Foo<int> { void bar() {} };
        void foo() {
          Fo^o<char> abc;
          Foo<int> b;
        }
      )cpp",

      R"cpp(// partial template specialization
        template <typename T>
        struct Foo { void bar() {} };
        template <typename T>
        struct [[Foo]]<T*> { void bar() {} };
        ^Foo<int*> x;
      )cpp",

      R"cpp(// function template specializations
        template <class T>
        void foo(T) {}
        template <>
        void [[foo]](int) {}
        void bar() {
          fo^o(10);
        }
      )cpp",

      R"cpp(// variable template decls
        template <class T>
        T var = T();

        template <>
        double [[var]]<int> = 10;

        double y = va^r<int>;
      )cpp",

      R"cpp(// No implicit constructors
        class X {
          X(X&& x) = default;
        };
        X [[makeX]]() {}
        void foo() {
          auto x = m^akeX();
        }
      )cpp",

      R"cpp(
        struct X {
          X& [[operator]]++() {}
        };
        void foo(X& x) {
          +^+x;
        }
      )cpp",

      R"cpp(// Declaration of explicit template specialization
        template <typename T>
        struct $decl[[Foo]] {};

        template <>
        struct Fo^o<int> {};
      )cpp",

      R"cpp(// Declaration of partial template specialization
        template <typename T>
        struct $decl[[Foo]] {};

        template <typename T>
        struct Fo^o<T*> {};
      )cpp"};
  for (const char *Test : Tests) {
    Annotations T(Test);
    llvm::Optional<Range> WantDecl;
    llvm::Optional<Range> WantDef;
    if (!T.ranges().empty())
      WantDecl = WantDef = T.range();
    if (!T.ranges("decl").empty())
      WantDecl = T.range("decl");
    if (!T.ranges("def").empty())
      WantDef = T.range("def");

    TestTU TU;
    TU.Code = T.code();

    // FIXME: Auto-completion in a template requires disabling delayed template
    // parsing.
    TU.ExtraArgs.push_back("-fno-delayed-template-parsing");

    auto AST = TU.build();
    auto Results = locateSymbolAt(AST, T.point());

    if (!WantDecl) {
      EXPECT_THAT(Results, IsEmpty()) << Test;
    } else {
      ASSERT_THAT(Results, ::testing::SizeIs(1)) << Test;
      EXPECT_EQ(Results[0].PreferredDeclaration.range, *WantDecl) << Test;
      llvm::Optional<Range> GotDef;
      if (Results[0].Definition)
        GotDef = Results[0].Definition->range;
      EXPECT_EQ(WantDef, GotDef) << Test;
    }
  }
}

TEST(LocateSymbol, Ambiguous) {
  auto T = Annotations(R"cpp(
    struct Foo {
      Foo();
      Foo(Foo&&);
      $ConstructorLoc[[Foo]](const char*);
    };

    Foo f();

    void g(Foo foo);

    void call() {
      const char* str = "123";
      Foo a = $1^str;
      Foo b = Foo($2^str);
      Foo c = $3^f();
      $4^g($5^f());
      g($6^str);
      Foo ab$7^c;
      Foo ab$8^cd("asdf");
      Foo foox = Fo$9^o("asdf");
      Foo abcde$10^("asdf");
      Foo foox2 = Foo$11^("asdf");
    }
  )cpp");
  auto AST = TestTU::withCode(T.code()).build();
  // Ordered assertions are deliberate: we expect a predictable order.
  EXPECT_THAT(locateSymbolAt(AST, T.point("1")), ElementsAre(Sym("str")));
  EXPECT_THAT(locateSymbolAt(AST, T.point("2")), ElementsAre(Sym("str")));
  EXPECT_THAT(locateSymbolAt(AST, T.point("3")), ElementsAre(Sym("f")));
  EXPECT_THAT(locateSymbolAt(AST, T.point("4")), ElementsAre(Sym("g")));
  EXPECT_THAT(locateSymbolAt(AST, T.point("5")), ElementsAre(Sym("f")));
  EXPECT_THAT(locateSymbolAt(AST, T.point("6")), ElementsAre(Sym("str")));
  // FIXME: Target the constructor as well.
  EXPECT_THAT(locateSymbolAt(AST, T.point("7")), ElementsAre(Sym("abc")));
  // FIXME: Target the constructor as well.
  EXPECT_THAT(locateSymbolAt(AST, T.point("8")), ElementsAre(Sym("abcd")));
  // FIXME: Target the constructor as well.
  EXPECT_THAT(locateSymbolAt(AST, T.point("9")), ElementsAre(Sym("Foo")));
  EXPECT_THAT(locateSymbolAt(AST, T.point("10")),
              ElementsAre(Sym("Foo", T.range("ConstructorLoc"))));
  EXPECT_THAT(locateSymbolAt(AST, T.point("11")),
              ElementsAre(Sym("Foo", T.range("ConstructorLoc"))));
}

TEST(LocateSymbol, TemplateTypedefs) {
  auto T = Annotations(R"cpp(
    template <class T> struct function {};
    template <class T> using callback = function<T()>;

    c^allback<int> foo;
  )cpp");
  auto AST = TestTU::withCode(T.code()).build();
  EXPECT_THAT(locateSymbolAt(AST, T.point()), ElementsAre(Sym("callback")));
}

TEST(LocateSymbol, RelPathsInCompileCommand) {
  // The source is in "/clangd-test/src".
  // We build in "/clangd-test/build".

  Annotations SourceAnnotations(R"cpp(
#include "header_in_preamble.h"
int [[foo]];
#include "header_not_in_preamble.h"
int baz = f$p1^oo + bar_pre$p2^amble + bar_not_pre$p3^amble;
)cpp");

  Annotations HeaderInPreambleAnnotations(R"cpp(
int [[bar_preamble]];
)cpp");

  Annotations HeaderNotInPreambleAnnotations(R"cpp(
int [[bar_not_preamble]];
)cpp");

  // Make the compilation paths appear as ../src/foo.cpp in the compile
  // commands.
  SmallString<32> RelPathPrefix("..");
  llvm::sys::path::append(RelPathPrefix, "src");
  std::string BuildDir = testPath("build");
  MockCompilationDatabase CDB(BuildDir, RelPathPrefix);

  IgnoreDiagnostics DiagConsumer;
  MockFSProvider FS;
  ClangdServer Server(CDB, FS, DiagConsumer, ClangdServer::optsForTest());

  // Fill the filesystem.
  auto FooCpp = testPath("src/foo.cpp");
  FS.Files[FooCpp] = "";
  auto HeaderInPreambleH = testPath("src/header_in_preamble.h");
  FS.Files[HeaderInPreambleH] = HeaderInPreambleAnnotations.code();
  auto HeaderNotInPreambleH = testPath("src/header_not_in_preamble.h");
  FS.Files[HeaderNotInPreambleH] = HeaderNotInPreambleAnnotations.code();

  runAddDocument(Server, FooCpp, SourceAnnotations.code());

  // Go to a definition in main source file.
  auto Locations =
      runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("p1"));
  EXPECT_TRUE(bool(Locations)) << "findDefinitions returned an error";
  EXPECT_THAT(*Locations, ElementsAre(Sym("foo", SourceAnnotations.range())));

  // Go to a definition in header_in_preamble.h.
  Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("p2"));
  EXPECT_TRUE(bool(Locations)) << "findDefinitions returned an error";
  EXPECT_THAT(
      *Locations,
      ElementsAre(Sym("bar_preamble", HeaderInPreambleAnnotations.range())));

  // Go to a definition in header_not_in_preamble.h.
  Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("p3"));
  EXPECT_TRUE(bool(Locations)) << "findDefinitions returned an error";
  EXPECT_THAT(*Locations,
              ElementsAre(Sym("bar_not_preamble",
                              HeaderNotInPreambleAnnotations.range())));
}

TEST(GoToInclude, All) {
  MockFSProvider FS;
  IgnoreDiagnostics DiagConsumer;
  MockCompilationDatabase CDB;
  ClangdServer Server(CDB, FS, DiagConsumer, ClangdServer::optsForTest());

  auto FooCpp = testPath("foo.cpp");
  const char *SourceContents = R"cpp(
  #include ^"$2^foo.h$3^"
  #include "$4^invalid.h"
  int b = a;
  // test
  int foo;
  #in$5^clude "$6^foo.h"$7^
  )cpp";
  Annotations SourceAnnotations(SourceContents);
  FS.Files[FooCpp] = SourceAnnotations.code();
  auto FooH = testPath("foo.h");

  const char *HeaderContents = R"cpp([[]]#pragma once
                                     int a;
                                     )cpp";
  Annotations HeaderAnnotations(HeaderContents);
  FS.Files[FooH] = HeaderAnnotations.code();

  Server.addDocument(FooH, HeaderAnnotations.code());
  Server.addDocument(FooCpp, SourceAnnotations.code());

  // Test include in preamble.
  auto Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point());
  ASSERT_TRUE(bool(Locations)) << "locateSymbolAt returned an error";
  EXPECT_THAT(*Locations, ElementsAre(Sym("foo.h", HeaderAnnotations.range())));

  // Test include in preamble, last char.
  Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("2"));
  ASSERT_TRUE(bool(Locations)) << "locateSymbolAt returned an error";
  EXPECT_THAT(*Locations, ElementsAre(Sym("foo.h", HeaderAnnotations.range())));

  Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("3"));
  ASSERT_TRUE(bool(Locations)) << "locateSymbolAt returned an error";
  EXPECT_THAT(*Locations, ElementsAre(Sym("foo.h", HeaderAnnotations.range())));

  // Test include outside of preamble.
  Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("6"));
  ASSERT_TRUE(bool(Locations)) << "locateSymbolAt returned an error";
  EXPECT_THAT(*Locations, ElementsAre(Sym("foo.h", HeaderAnnotations.range())));

  // Test a few positions that do not result in Locations.
  Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("4"));
  ASSERT_TRUE(bool(Locations)) << "locateSymbolAt returned an error";
  EXPECT_THAT(*Locations, IsEmpty());

  Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("5"));
  ASSERT_TRUE(bool(Locations)) << "locateSymbolAt returned an error";
  EXPECT_THAT(*Locations, ElementsAre(Sym("foo.h", HeaderAnnotations.range())));

  Locations = runLocateSymbolAt(Server, FooCpp, SourceAnnotations.point("7"));
  ASSERT_TRUE(bool(Locations)) << "locateSymbolAt returned an error";
  EXPECT_THAT(*Locations, ElementsAre(Sym("foo.h", HeaderAnnotations.range())));

  // Objective C #import directive.
  Annotations ObjC(R"objc(
  #import "^foo.h"
  )objc");
  auto FooM = testPath("foo.m");
  FS.Files[FooM] = ObjC.code();

  Server.addDocument(FooM, ObjC.code());
  Locations = runLocateSymbolAt(Server, FooM, ObjC.point());
  ASSERT_TRUE(bool(Locations)) << "locateSymbolAt returned an error";
  EXPECT_THAT(*Locations, ElementsAre(Sym("foo.h", HeaderAnnotations.range())));
}

TEST(LocateSymbol, WithPreamble) {
  // Test stragety: AST should always use the latest preamble instead of last
  // good preamble.
  MockFSProvider FS;
  IgnoreDiagnostics DiagConsumer;
  MockCompilationDatabase CDB;
  ClangdServer Server(CDB, FS, DiagConsumer, ClangdServer::optsForTest());

  auto FooCpp = testPath("foo.cpp");
  // The trigger locations must be the same.
  Annotations FooWithHeader(R"cpp(#include "fo^o.h")cpp");
  Annotations FooWithoutHeader(R"cpp(double    [[fo^o]]();)cpp");

  FS.Files[FooCpp] = FooWithHeader.code();

  auto FooH = testPath("foo.h");
  Annotations FooHeader(R"cpp([[]])cpp");
  FS.Files[FooH] = FooHeader.code();

  runAddDocument(Server, FooCpp, FooWithHeader.code());
  // LocateSymbol goes to a #include file: the result comes from the preamble.
  EXPECT_THAT(
      cantFail(runLocateSymbolAt(Server, FooCpp, FooWithHeader.point())),
      ElementsAre(Sym("foo.h", FooHeader.range())));

  // Only preamble is built, and no AST is built in this request.
  Server.addDocument(FooCpp, FooWithoutHeader.code(), WantDiagnostics::No);
  // We build AST here, and it should use the latest preamble rather than the
  // stale one.
  EXPECT_THAT(
      cantFail(runLocateSymbolAt(Server, FooCpp, FooWithoutHeader.point())),
      ElementsAre(Sym("foo", FooWithoutHeader.range())));

  // Reset test environment.
  runAddDocument(Server, FooCpp, FooWithHeader.code());
  // Both preamble and AST are built in this request.
  Server.addDocument(FooCpp, FooWithoutHeader.code(), WantDiagnostics::Yes);
  // Use the AST being built in above request.
  EXPECT_THAT(
      cantFail(runLocateSymbolAt(Server, FooCpp, FooWithoutHeader.point())),
      ElementsAre(Sym("foo", FooWithoutHeader.range())));
}

TEST(FindReferences, WithinAST) {
  const char *Tests[] = {
      R"cpp(// Local variable
        int main() {
          int [[foo]];
          [[^foo]] = 2;
          int test1 = [[foo]];
        }
      )cpp",

      R"cpp(// Struct
        namespace ns1 {
        struct [[Foo]] {};
        } // namespace ns1
        int main() {
          ns1::[[Fo^o]]* Params;
        }
      )cpp",

      R"cpp(// Forward declaration
        class [[Foo]];
        class [[Foo]] {}
        int main() {
          [[Fo^o]] foo;
        }
      )cpp",

      R"cpp(// Function
        int [[foo]](int) {}
        int main() {
          auto *X = &[[^foo]];
          [[foo]](42)
        }
      )cpp",

      R"cpp(// Field
        struct Foo {
          int [[foo]];
          Foo() : [[foo]](0) {}
        };
        int main() {
          Foo f;
          f.[[f^oo]] = 1;
        }
      )cpp",

      R"cpp(// Method call
        struct Foo { int [[foo]](); };
        int Foo::[[foo]]() {}
        int main() {
          Foo f;
          f.[[^foo]]();
        }
      )cpp",

      R"cpp(// Constructor
        struct Foo {
          [[F^oo]](int);
        };
        void foo() {
          Foo f = [[Foo]](42);
        }
      )cpp",

      R"cpp(// Typedef
        typedef int [[Foo]];
        int main() {
          [[^Foo]] bar;
        }
      )cpp",

      R"cpp(// Namespace
        namespace [[ns]] {
        struct Foo {};
        } // namespace ns
        int main() { [[^ns]]::Foo foo; }
      )cpp",
  };
  for (const char *Test : Tests) {
    Annotations T(Test);
    auto AST = TestTU::withCode(T.code()).build();
    std::vector<Matcher<Location>> ExpectedLocations;
    for (const auto &R : T.ranges())
      ExpectedLocations.push_back(RangeIs(R));
    EXPECT_THAT(findReferences(AST, T.point(), 0).References,
                ElementsAreArray(ExpectedLocations))
        << Test;
  }
}

TEST(FindReferences, ExplicitSymbols) {
  const char *Tests[] = {
      R"cpp(
      struct Foo { Foo* [[self]]() const; };
      void f() {
        Foo foo;
        if (Foo* T = foo.[[^self]]()) {} // Foo member call expr.
      }
      )cpp",

      R"cpp(
      struct Foo { Foo(int); };
      Foo f() {
        int [[b]];
        return [[^b]]; // Foo constructor expr.
      }
      )cpp",

      R"cpp(
      struct Foo {};
      void g(Foo);
      Foo [[f]]();
      void call() {
        g([[^f]]());  // Foo constructor expr.
      }
      )cpp",

      R"cpp(
      void [[foo]](int);
      void [[foo]](double);

      namespace ns {
      using ::[[fo^o]];
      }
      )cpp",

      R"cpp(
      struct X {
        operator bool();
      };

      int test() {
        X [[a]];
        [[a]].operator bool();
        if ([[a^]]) {} // ignore implicit conversion-operator AST node
      }
    )cpp",
  };
  for (const char *Test : Tests) {
    Annotations T(Test);
    auto AST = TestTU::withCode(T.code()).build();
    std::vector<Matcher<Location>> ExpectedLocations;
    for (const auto &R : T.ranges())
      ExpectedLocations.push_back(RangeIs(R));
    ASSERT_THAT(ExpectedLocations, Not(IsEmpty()));
    EXPECT_THAT(findReferences(AST, T.point(), 0).References,
                ElementsAreArray(ExpectedLocations))
        << Test;
  }
}

TEST(FindReferences, NeedsIndex) {
  const char *Header = "int foo();";
  Annotations Main("int main() { [[f^oo]](); }");
  TestTU TU;
  TU.Code = Main.code();
  TU.HeaderCode = Header;
  auto AST = TU.build();

  // References in main file are returned without index.
  EXPECT_THAT(
      findReferences(AST, Main.point(), 0, /*Index=*/nullptr).References,
      ElementsAre(RangeIs(Main.range())));
  Annotations IndexedMain(R"cpp(
    int main() { [[f^oo]](); }
  )cpp");

  // References from indexed files are included.
  TestTU IndexedTU;
  IndexedTU.Code = IndexedMain.code();
  IndexedTU.Filename = "Indexed.cpp";
  IndexedTU.HeaderCode = Header;
  EXPECT_THAT(
      findReferences(AST, Main.point(), 0, IndexedTU.index().get()).References,
      ElementsAre(RangeIs(Main.range()), RangeIs(IndexedMain.range())));
  auto LimitRefs =
      findReferences(AST, Main.point(), /*Limit*/ 1, IndexedTU.index().get());
  EXPECT_EQ(1u, LimitRefs.References.size());
  EXPECT_TRUE(LimitRefs.HasMore);

  // If the main file is in the index, we don't return duplicates.
  // (even if the references are in a different location)
  TU.Code = ("\n\n" + Main.code()).str();
  EXPECT_THAT(findReferences(AST, Main.point(), 0, TU.index().get()).References,
              ElementsAre(RangeIs(Main.range())));
}

TEST(FindReferences, NoQueryForLocalSymbols) {
  struct RecordingIndex : public MemIndex {
    mutable Optional<llvm::DenseSet<SymbolID>> RefIDs;
    bool refs(const RefsRequest &Req,
              llvm::function_ref<void(const Ref &)>) const override {
      RefIDs = Req.IDs;
      return false;
    }
  };

  struct Test {
    StringRef AnnotatedCode;
    bool WantQuery;
  } Tests[] = {
      {"int ^x;", true},
      // For now we don't assume header structure which would allow skipping.
      {"namespace { int ^x; }", true},
      {"static int ^x;", true},
      // Anything in a function certainly can't be referenced though.
      {"void foo() { int ^x; }", false},
      {"void foo() { struct ^x{}; }", false},
      {"auto lambda = []{ int ^x; };", false},
  };
  for (Test T : Tests) {
    Annotations File(T.AnnotatedCode);
    RecordingIndex Rec;
    auto AST = TestTU::withCode(File.code()).build();
    findReferences(AST, File.point(), 0, &Rec);
    if (T.WantQuery)
      EXPECT_NE(Rec.RefIDs, None) << T.AnnotatedCode;
    else
      EXPECT_EQ(Rec.RefIDs, None) << T.AnnotatedCode;
  }
}

TEST(GetNonLocalDeclRefs, All) {
  struct Case {
    llvm::StringRef AnnotatedCode;
    std::vector<llvm::StringRef> ExpectedDecls;
  } Cases[] = {
      {
          // VarDecl and ParamVarDecl
          R"cpp(
            void bar();
            void ^foo(int baz) {
              int x = 10;
              bar();
            })cpp",
          {"bar"},
      },
      {
          // Method from class
          R"cpp(
            class Foo { public: void foo(); };
            class Bar {
              void foo();
              void bar();
            };
            void Bar::^foo() {
              Foo f;
              bar();
              f.foo();
            })cpp",
          {"Bar", "Bar::bar", "Foo", "Foo::foo"},
      },
      {
          // Local types
          R"cpp(
            void ^foo() {
              class Foo { public: void foo() {} };
              class Bar { public: void bar() {} };
              Foo f;
              Bar b;
              b.bar();
              f.foo();
            })cpp",
          {},
      },
      {
          // Template params
          R"cpp(
            template <typename T, template<typename> class Q>
            void ^foo() {
              T x;
              Q<T> y;
            })cpp",
          {},
      },
  };
  for (const Case &C : Cases) {
    Annotations File(C.AnnotatedCode);
    auto AST = TestTU::withCode(File.code()).build();
    ASSERT_TRUE(AST.getDiagnostics().empty())
        << AST.getDiagnostics().begin()->Message;
    SourceLocation SL = llvm::cantFail(
        sourceLocationInMainFile(AST.getSourceManager(), File.point()));

    const FunctionDecl *FD =
        llvm::dyn_cast<FunctionDecl>(&findDecl(AST, [SL](const NamedDecl &ND) {
          return ND.getLocation() == SL && llvm::isa<FunctionDecl>(ND);
        }));
    ASSERT_NE(FD, nullptr);

    auto NonLocalDeclRefs = getNonLocalDeclRefs(AST, FD);
    std::vector<std::string> Names;
    for (const Decl *D : NonLocalDeclRefs) {
      if (const auto *ND = llvm::dyn_cast<NamedDecl>(D))
        Names.push_back(ND->getQualifiedNameAsString());
    }
    EXPECT_THAT(Names, UnorderedElementsAreArray(C.ExpectedDecls))
        << File.code();
  }
}

} // namespace
} // namespace clangd
} // namespace clang
