//===-- ChangeNamespace.cpp - Change namespace implementation -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "ChangeNamespace.h"
#include "clang/Format/Format.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/ErrorHandling.h"

using namespace clang::ast_matchers;

namespace clang {
namespace change_namespace {

namespace {

inline std::string
joinNamespaces(const llvm::SmallVectorImpl<StringRef> &Namespaces) {
  if (Namespaces.empty())
    return "";
  std::string Result = Namespaces.front();
  for (auto I = Namespaces.begin() + 1, E = Namespaces.end(); I != E; ++I)
    Result += ("::" + *I).str();
  return Result;
}

SourceLocation startLocationForType(TypeLoc TLoc) {
  // For elaborated types (e.g. `struct a::A`) we want the portion after the
  // `struct` but including the namespace qualifier, `a::`.
  if (TLoc.getTypeLocClass() == TypeLoc::Elaborated) {
    NestedNameSpecifierLoc NestedNameSpecifier =
        TLoc.castAs<ElaboratedTypeLoc>().getQualifierLoc();
    if (NestedNameSpecifier.getNestedNameSpecifier())
      return NestedNameSpecifier.getBeginLoc();
    TLoc = TLoc.getNextTypeLoc();
  }
  return TLoc.getLocStart();
}

SourceLocation endLocationForType(TypeLoc TLoc) {
  // Dig past any namespace or keyword qualifications.
  while (TLoc.getTypeLocClass() == TypeLoc::Elaborated ||
         TLoc.getTypeLocClass() == TypeLoc::Qualified)
    TLoc = TLoc.getNextTypeLoc();

  // The location for template specializations (e.g. Foo<int>) includes the
  // templated types in its location range.  We want to restrict this to just
  // before the `<` character.
  if (TLoc.getTypeLocClass() == TypeLoc::TemplateSpecialization)
    return TLoc.castAs<TemplateSpecializationTypeLoc>()
        .getLAngleLoc()
        .getLocWithOffset(-1);
  return TLoc.getEndLoc();
}

// Returns the containing namespace of `InnerNs` by skipping `PartialNsName`.
// If the `InnerNs` does not have `PartialNsName` as suffix, or `PartialNsName`
// is empty, nullptr is returned.
// For example, if `InnerNs` is "a::b::c" and `PartialNsName` is "b::c", then
// the NamespaceDecl of namespace "a" will be returned.
const NamespaceDecl *getOuterNamespace(const NamespaceDecl *InnerNs,
                                       llvm::StringRef PartialNsName) {
  if (!InnerNs || PartialNsName.empty())
    return nullptr;
  const auto *CurrentContext = llvm::cast<DeclContext>(InnerNs);
  const auto *CurrentNs = InnerNs;
  llvm::SmallVector<llvm::StringRef, 4> PartialNsNameSplitted;
  PartialNsName.split(PartialNsNameSplitted, "::", /*MaxSplit=*/-1,
                      /*KeepEmpty=*/false);
  while (!PartialNsNameSplitted.empty()) {
    // Get the inner-most namespace in CurrentContext.
    while (CurrentContext && !llvm::isa<NamespaceDecl>(CurrentContext))
      CurrentContext = CurrentContext->getParent();
    if (!CurrentContext)
      return nullptr;
    CurrentNs = llvm::cast<NamespaceDecl>(CurrentContext);
    if (PartialNsNameSplitted.back() != CurrentNs->getNameAsString())
      return nullptr;
    PartialNsNameSplitted.pop_back();
    CurrentContext = CurrentContext->getParent();
  }
  return CurrentNs;
}

static std::unique_ptr<Lexer>
getLexerStartingFromLoc(SourceLocation Loc, const SourceManager &SM,
                        const LangOptions &LangOpts) {
  if (Loc.isMacroID() &&
      !Lexer::isAtEndOfMacroExpansion(Loc, SM, LangOpts, &Loc))
    return nullptr;
  // Break down the source location.
  std::pair<FileID, unsigned> LocInfo = SM.getDecomposedLoc(Loc);
  // Try to load the file buffer.
  bool InvalidTemp = false;
  llvm::StringRef File = SM.getBufferData(LocInfo.first, &InvalidTemp);
  if (InvalidTemp)
    return nullptr;

  const char *TokBegin = File.data() + LocInfo.second;
  // Lex from the start of the given location.
  return llvm::make_unique<Lexer>(SM.getLocForStartOfFile(LocInfo.first),
                                  LangOpts, File.begin(), TokBegin, File.end());
}

// FIXME: get rid of this helper function if this is supported in clang-refactor
// library.
static SourceLocation getStartOfNextLine(SourceLocation Loc,
                                         const SourceManager &SM,
                                         const LangOptions &LangOpts) {
  std::unique_ptr<Lexer> Lex = getLexerStartingFromLoc(Loc, SM, LangOpts);
  if (!Lex.get())
    return SourceLocation();
  llvm::SmallVector<char, 16> Line;
  // FIXME: this is a bit hacky to get ReadToEndOfLine work.
  Lex->setParsingPreprocessorDirective(true);
  Lex->ReadToEndOfLine(&Line);
  auto End = Loc.getLocWithOffset(Line.size());
  return SM.getLocForEndOfFile(SM.getDecomposedLoc(Loc).first) == End
             ? End
             : End.getLocWithOffset(1);
}

// Returns `R` with new range that refers to code after `Replaces` being
// applied.
tooling::Replacement
getReplacementInChangedCode(const tooling::Replacements &Replaces,
                            const tooling::Replacement &R) {
  unsigned NewStart = Replaces.getShiftedCodePosition(R.getOffset());
  unsigned NewEnd =
      Replaces.getShiftedCodePosition(R.getOffset() + R.getLength());
  return tooling::Replacement(R.getFilePath(), NewStart, NewEnd - NewStart,
                              R.getReplacementText());
}

// Adds a replacement `R` into `Replaces` or merges it into `Replaces` by
// applying all existing Replaces first if there is conflict.
void addOrMergeReplacement(const tooling::Replacement &R,
                           tooling::Replacements *Replaces) {
  auto Err = Replaces->add(R);
  if (Err) {
    llvm::consumeError(std::move(Err));
    auto Replace = getReplacementInChangedCode(*Replaces, R);
    *Replaces = Replaces->merge(tooling::Replacements(Replace));
  }
}

tooling::Replacement createReplacement(SourceLocation Start, SourceLocation End,
                                       llvm::StringRef ReplacementText,
                                       const SourceManager &SM) {
  if (!Start.isValid() || !End.isValid()) {
    llvm::errs() << "start or end location were invalid\n";
    return tooling::Replacement();
  }
  if (SM.getDecomposedLoc(Start).first != SM.getDecomposedLoc(End).first) {
    llvm::errs()
        << "start or end location were in different macro expansions\n";
    return tooling::Replacement();
  }
  Start = SM.getSpellingLoc(Start);
  End = SM.getSpellingLoc(End);
  if (SM.getFileID(Start) != SM.getFileID(End)) {
    llvm::errs() << "start or end location were in different files\n";
    return tooling::Replacement();
  }
  return tooling::Replacement(
      SM, CharSourceRange::getTokenRange(SM.getSpellingLoc(Start),
                                         SM.getSpellingLoc(End)),
      ReplacementText);
}

tooling::Replacement createInsertion(SourceLocation Loc,
                                     llvm::StringRef InsertText,
                                     const SourceManager &SM) {
  if (Loc.isInvalid()) {
    llvm::errs() << "insert Location is invalid.\n";
    return tooling::Replacement();
  }
  Loc = SM.getSpellingLoc(Loc);
  return tooling::Replacement(SM, Loc, 0, InsertText);
}

// Returns the shortest qualified name for declaration `DeclName` in the
// namespace `NsName`. For example, if `DeclName` is "a::b::X" and `NsName`
// is "a::c::d", then "b::X" will be returned.
// \param DeclName A fully qualified name, "::a::b::X" or "a::b::X".
// \param NsName A fully qualified name, "::a::b" or "a::b". Global namespace
//        will have empty name.
std::string getShortestQualifiedNameInNamespace(llvm::StringRef DeclName,
                                                llvm::StringRef NsName) {
  DeclName = DeclName.ltrim(':');
  NsName = NsName.ltrim(':');
  if (DeclName.find(':') == llvm::StringRef::npos)
    return DeclName;

  while (!DeclName.consume_front((NsName + "::").str())) {
    const auto Pos = NsName.find_last_of(':');
    if (Pos == llvm::StringRef::npos)
      return DeclName;
    assert(Pos > 0);
    NsName = NsName.substr(0, Pos - 1);
  }
  return DeclName;
}

std::string wrapCodeInNamespace(StringRef NestedNs, std::string Code) {
  if (Code.back() != '\n')
    Code += "\n";
  llvm::SmallVector<StringRef, 4> NsSplitted;
  NestedNs.split(NsSplitted, "::", /*MaxSplit=*/-1,
                 /*KeepEmpty=*/false);
  while (!NsSplitted.empty()) {
    // FIXME: consider code style for comments.
    Code = ("namespace " + NsSplitted.back() + " {\n" + Code +
            "} // namespace " + NsSplitted.back() + "\n")
               .str();
    NsSplitted.pop_back();
  }
  return Code;
}

// Returns true if \p D is a nested DeclContext in \p Context
bool isNestedDeclContext(const DeclContext *D, const DeclContext *Context) {
  while (D) {
    if (D == Context)
      return true;
    D = D->getParent();
  }
  return false;
}

// Returns true if \p D is visible at \p Loc with DeclContext \p DeclCtx.
bool isDeclVisibleAtLocation(const SourceManager &SM, const Decl *D,
                             const DeclContext *DeclCtx, SourceLocation Loc) {
  SourceLocation DeclLoc = SM.getSpellingLoc(D->getLocation());
  Loc = SM.getSpellingLoc(Loc);
  return SM.isBeforeInTranslationUnit(DeclLoc, Loc) &&
         (SM.getFileID(DeclLoc) == SM.getFileID(Loc) &&
          isNestedDeclContext(DeclCtx, D->getDeclContext()));
}

} // anonymous namespace

ChangeNamespaceTool::ChangeNamespaceTool(
    llvm::StringRef OldNs, llvm::StringRef NewNs, llvm::StringRef FilePattern,
    std::map<std::string, tooling::Replacements> *FileToReplacements,
    llvm::StringRef FallbackStyle)
    : FallbackStyle(FallbackStyle), FileToReplacements(*FileToReplacements),
      OldNamespace(OldNs.ltrim(':')), NewNamespace(NewNs.ltrim(':')),
      FilePattern(FilePattern), FilePatternRE(FilePattern) {
  FileToReplacements->clear();
  llvm::SmallVector<llvm::StringRef, 4> OldNsSplitted;
  llvm::SmallVector<llvm::StringRef, 4> NewNsSplitted;
  llvm::StringRef(OldNamespace).split(OldNsSplitted, "::");
  llvm::StringRef(NewNamespace).split(NewNsSplitted, "::");
  // Calculates `DiffOldNamespace` and `DiffNewNamespace`.
  while (!OldNsSplitted.empty() && !NewNsSplitted.empty() &&
         OldNsSplitted.front() == NewNsSplitted.front()) {
    OldNsSplitted.erase(OldNsSplitted.begin());
    NewNsSplitted.erase(NewNsSplitted.begin());
  }
  DiffOldNamespace = joinNamespaces(OldNsSplitted);
  DiffNewNamespace = joinNamespaces(NewNsSplitted);
}

void ChangeNamespaceTool::registerMatchers(ast_matchers::MatchFinder *Finder) {
  std::string FullOldNs = "::" + OldNamespace;
  // Prefix is the outer-most namespace in DiffOldNamespace. For example, if the
  // OldNamespace is "a::b::c" and DiffOldNamespace is "b::c", then Prefix will
  // be "a::b". Declarations in this namespace will not be visible in the new
  // namespace. If DiffOldNamespace is empty, Prefix will be a invalid name "-".
  llvm::SmallVector<llvm::StringRef, 4> DiffOldNsSplitted;
  llvm::StringRef(DiffOldNamespace)
      .split(DiffOldNsSplitted, "::", /*MaxSplit=*/-1,
             /*KeepEmpty=*/false);
  std::string Prefix = "-";
  if (!DiffOldNsSplitted.empty())
    Prefix = (StringRef(FullOldNs).drop_back(DiffOldNamespace.size()) +
              DiffOldNsSplitted.front())
                 .str();
  auto IsInMovedNs =
      allOf(hasAncestor(namespaceDecl(hasName(FullOldNs)).bind("ns_decl")),
            isExpansionInFileMatching(FilePattern));
  auto IsVisibleInNewNs = anyOf(
      IsInMovedNs, unless(hasAncestor(namespaceDecl(hasName(Prefix)))));
  // Match using declarations.
  Finder->addMatcher(
      usingDecl(isExpansionInFileMatching(FilePattern), IsVisibleInNewNs)
          .bind("using"),
      this);
  // Match using namespace declarations.
  Finder->addMatcher(usingDirectiveDecl(isExpansionInFileMatching(FilePattern),
                                        IsVisibleInNewNs)
                         .bind("using_namespace"),
                     this);

  // Match old namespace blocks.
  Finder->addMatcher(
      namespaceDecl(hasName(FullOldNs), isExpansionInFileMatching(FilePattern))
          .bind("old_ns"),
      this);

  // Match forward-declarations in the old namespace.
  Finder->addMatcher(
      cxxRecordDecl(unless(anyOf(isImplicit(), isDefinition())), IsInMovedNs)
          .bind("fwd_decl"),
      this);

  // Match references to types that are not defined in the old namespace.
  // Forward-declarations in the old namespace are also matched since they will
  // be moved back to the old namespace.
  auto DeclMatcher = namedDecl(
      hasAncestor(namespaceDecl()),
      unless(anyOf(
          isImplicit(), hasAncestor(namespaceDecl(isAnonymous())),
          hasAncestor(cxxRecordDecl()),
          allOf(IsInMovedNs, unless(cxxRecordDecl(unless(isDefinition())))))));

  // Match TypeLocs on the declaration. Carefully match only the outermost
  // TypeLoc and template specialization arguments (which are not outermost)
  // that are directly linked to types matching `DeclMatcher`. Nested name
  // specifier locs are handled separately below.
  Finder->addMatcher(
      typeLoc(IsInMovedNs,
              loc(qualType(hasDeclaration(DeclMatcher.bind("from_decl")))),
              unless(anyOf(hasParent(typeLoc(loc(qualType(
                               allOf(hasDeclaration(DeclMatcher),
                                     unless(templateSpecializationType())))))),
                           hasParent(nestedNameSpecifierLoc()))),
              hasAncestor(decl().bind("dc")))
          .bind("type"),
      this);

  // Types in `UsingShadowDecl` is not matched by `typeLoc` above, so we need to
  // special case it.
  Finder->addMatcher(usingDecl(IsInMovedNs, hasAnyUsingShadowDecl(decl()))
                         .bind("using_with_shadow"),
                     this);

  // Handle types in nested name specifier. Specifiers that are in a TypeLoc
  // matched above are not matched, e.g. "A::" in "A::A" is not matched since
  // "A::A" would have already been fixed.
  Finder->addMatcher(nestedNameSpecifierLoc(
                         hasAncestor(decl(IsInMovedNs).bind("dc")),
                         loc(nestedNameSpecifier(specifiesType(
                             hasDeclaration(DeclMatcher.bind("from_decl"))))),
                         unless(hasAncestor(typeLoc(loc(qualType(hasDeclaration(
                             decl(equalsBoundNode("from_decl")))))))))
                         .bind("nested_specifier_loc"),
                     this);

  // Matches base class initializers in constructors. TypeLocs of base class
  // initializers do not need to be fixed. For example,
  //    class X : public a::b::Y {
  //      public:
  //        X() : Y::Y() {} // Y::Y do not need namespace specifier.
  //    };
  Finder->addMatcher(
      cxxCtorInitializer(isBaseInitializer()).bind("base_initializer"), this);

  // Handle function.
  // Only handle functions that are defined in a namespace excluding member
  // function, static methods (qualified by nested specifier), and functions
  // defined in the global namespace.
  // Note that the matcher does not exclude calls to out-of-line static method
  // definitions, so we need to exclude them in the callback handler.
  auto FuncMatcher =
      functionDecl(unless(anyOf(cxxMethodDecl(), IsInMovedNs,
                                hasAncestor(namespaceDecl(isAnonymous())),
                                hasAncestor(cxxRecordDecl()))),
                   hasParent(namespaceDecl()));
  Finder->addMatcher(decl(forEachDescendant(expr(anyOf(
                              callExpr(callee(FuncMatcher)).bind("call"),
                              declRefExpr(to(FuncMatcher.bind("func_decl")))
                                  .bind("func_ref")))),
                          IsInMovedNs, unless(isImplicit()))
                         .bind("dc"),
                     this);

  auto GlobalVarMatcher = varDecl(
      hasGlobalStorage(), hasParent(namespaceDecl()),
      unless(anyOf(IsInMovedNs, hasAncestor(namespaceDecl(isAnonymous())))));
  Finder->addMatcher(declRefExpr(IsInMovedNs, hasAncestor(decl().bind("dc")),
                                 to(GlobalVarMatcher.bind("var_decl")))
                         .bind("var_ref"),
                     this);
}

void ChangeNamespaceTool::run(
    const ast_matchers::MatchFinder::MatchResult &Result) {
  if (const auto *Using = Result.Nodes.getNodeAs<UsingDecl>("using")) {
    UsingDecls.insert(Using);
  } else if (const auto *UsingNamespace =
                 Result.Nodes.getNodeAs<UsingDirectiveDecl>(
                     "using_namespace")) {
    UsingNamespaceDecls.insert(UsingNamespace);
  } else if (const auto *NsDecl =
                 Result.Nodes.getNodeAs<NamespaceDecl>("old_ns")) {
    moveOldNamespace(Result, NsDecl);
  } else if (const auto *FwdDecl =
                 Result.Nodes.getNodeAs<CXXRecordDecl>("fwd_decl")) {
    moveClassForwardDeclaration(Result, FwdDecl);
  } else if (const auto *UsingWithShadow =
                 Result.Nodes.getNodeAs<UsingDecl>("using_with_shadow")) {
    fixUsingShadowDecl(Result, UsingWithShadow);
  } else if (const auto *Specifier =
                 Result.Nodes.getNodeAs<NestedNameSpecifierLoc>(
                     "nested_specifier_loc")) {
    SourceLocation Start = Specifier->getBeginLoc();
    SourceLocation End = endLocationForType(Specifier->getTypeLoc());
    fixTypeLoc(Result, Start, End, Specifier->getTypeLoc());
  } else if (const auto *BaseInitializer =
                 Result.Nodes.getNodeAs<CXXCtorInitializer>(
                     "base_initializer")) {
    BaseCtorInitializerTypeLocs.push_back(
        BaseInitializer->getTypeSourceInfo()->getTypeLoc());
  } else if (const auto *TLoc = Result.Nodes.getNodeAs<TypeLoc>("type")) {
    fixTypeLoc(Result, startLocationForType(*TLoc), endLocationForType(*TLoc),
               *TLoc);
  } else if (const auto *VarRef =
                 Result.Nodes.getNodeAs<DeclRefExpr>("var_ref")) {
    const auto *Var = Result.Nodes.getNodeAs<VarDecl>("var_decl");
    assert(Var);
    if (Var->getCanonicalDecl()->isStaticDataMember())
      return;
    const auto *Context = Result.Nodes.getNodeAs<Decl>("dc");
    assert(Context && "Empty decl context.");
    fixDeclRefExpr(Result, Context->getDeclContext(),
                   llvm::cast<NamedDecl>(Var), VarRef);
  } else if (const auto *FuncRef =
                 Result.Nodes.getNodeAs<DeclRefExpr>("func_ref")) {
    const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func_decl");
    assert(Func);
    const auto *Context = Result.Nodes.getNodeAs<Decl>("dc");
    assert(Context && "Empty decl context.");
    fixDeclRefExpr(Result, Context->getDeclContext(),
                   llvm::cast<NamedDecl>(Func), FuncRef);
  } else {
    const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
    assert(Call != nullptr && "Expecting callback for CallExpr.");
    const FunctionDecl *Func = Call->getDirectCallee();
    assert(Func != nullptr);
    // Ignore out-of-line static methods since they will be handled by nested
    // name specifiers.
    if (Func->getCanonicalDecl()->getStorageClass() ==
            StorageClass::SC_Static &&
        Func->isOutOfLine())
      return;
    const auto *Context = Result.Nodes.getNodeAs<Decl>("dc");
    assert(Context && "Empty decl context.");
    SourceRange CalleeRange = Call->getCallee()->getSourceRange();
    replaceQualifiedSymbolInDeclContext(
        Result, Context->getDeclContext(), CalleeRange.getBegin(),
        CalleeRange.getEnd(), llvm::cast<NamedDecl>(Func));
  }
}

static SourceLocation getLocAfterNamespaceLBrace(const NamespaceDecl *NsDecl,
                                                 const SourceManager &SM,
                                                 const LangOptions &LangOpts) {
  std::unique_ptr<Lexer> Lex =
      getLexerStartingFromLoc(NsDecl->getLocStart(), SM, LangOpts);
  assert(Lex.get() &&
         "Failed to create lexer from the beginning of namespace.");
  if (!Lex.get())
    return SourceLocation();
  Token Tok;
  while (!Lex->LexFromRawLexer(Tok) && Tok.isNot(tok::TokenKind::l_brace)) {
  }
  return Tok.isNot(tok::TokenKind::l_brace)
             ? SourceLocation()
             : Tok.getEndLoc().getLocWithOffset(1);
}

// Stores information about a moved namespace in `MoveNamespaces` and leaves
// the actual movement to `onEndOfTranslationUnit()`.
void ChangeNamespaceTool::moveOldNamespace(
    const ast_matchers::MatchFinder::MatchResult &Result,
    const NamespaceDecl *NsDecl) {
  // If the namespace is empty, do nothing.
  if (Decl::castToDeclContext(NsDecl)->decls_empty())
    return;

  // Get the range of the code in the old namespace.
  SourceLocation Start = getLocAfterNamespaceLBrace(
      NsDecl, *Result.SourceManager, Result.Context->getLangOpts());
  assert(Start.isValid() && "Can't find l_brace for namespace.");
  SourceLocation End = NsDecl->getRBraceLoc().getLocWithOffset(-1);
  // Create a replacement that deletes the code in the old namespace merely for
  // retrieving offset and length from it.
  const auto R = createReplacement(Start, End, "", *Result.SourceManager);
  MoveNamespace MoveNs;
  MoveNs.Offset = R.getOffset();
  MoveNs.Length = R.getLength();

  // Insert the new namespace after `DiffOldNamespace`. For example, if
  // `OldNamespace` is "a::b::c" and `NewNamespace` is `a::x::y`, then
  // "x::y" will be inserted inside the existing namespace "a" and after "a::b".
  // `OuterNs` is the first namespace in `DiffOldNamespace`, e.g. "namespace b"
  // in the above example.
  // If there is no outer namespace (i.e. DiffOldNamespace is empty), the new
  // namespace will be a nested namespace in the old namespace.
  const NamespaceDecl *OuterNs = getOuterNamespace(NsDecl, DiffOldNamespace);
  SourceLocation InsertionLoc = Start;
  if (OuterNs) {
    SourceLocation LocAfterNs =
        getStartOfNextLine(OuterNs->getRBraceLoc(), *Result.SourceManager,
                           Result.Context->getLangOpts());
    assert(LocAfterNs.isValid() &&
           "Failed to get location after DiffOldNamespace");
    InsertionLoc = LocAfterNs;
  }
  MoveNs.InsertionOffset = Result.SourceManager->getFileOffset(
      Result.SourceManager->getSpellingLoc(InsertionLoc));
  MoveNs.FID = Result.SourceManager->getFileID(Start);
  MoveNs.SourceMgr = Result.SourceManager;
  MoveNamespaces[R.getFilePath()].push_back(MoveNs);
}

// Removes a class forward declaration from the code in the moved namespace and
// creates an `InsertForwardDeclaration` to insert the forward declaration back
// into the old namespace after moving code from the old namespace to the new
// namespace.
// For example, changing "a" to "x":
// Old code:
//   namespace a {
//   class FWD;
//   class A { FWD *fwd; }
//   }  // a
// New code:
//   namespace a {
//   class FWD;
//   }  // a
//   namespace x {
//   class A { a::FWD *fwd; }
//   }  // x
void ChangeNamespaceTool::moveClassForwardDeclaration(
    const ast_matchers::MatchFinder::MatchResult &Result,
    const CXXRecordDecl *FwdDecl) {
  SourceLocation Start = FwdDecl->getLocStart();
  SourceLocation End = FwdDecl->getLocEnd();
  SourceLocation AfterSemi = Lexer::findLocationAfterToken(
      End, tok::semi, *Result.SourceManager, Result.Context->getLangOpts(),
      /*SkipTrailingWhitespaceAndNewLine=*/true);
  if (AfterSemi.isValid())
    End = AfterSemi.getLocWithOffset(-1);
  // Delete the forward declaration from the code to be moved.
  const auto Deletion =
      createReplacement(Start, End, "", *Result.SourceManager);
  auto Err = FileToReplacements[Deletion.getFilePath()].add(Deletion);
  if (Err)
    llvm_unreachable(llvm::toString(std::move(Err)).c_str());
  llvm::StringRef Code = Lexer::getSourceText(
      CharSourceRange::getTokenRange(
          Result.SourceManager->getSpellingLoc(Start),
          Result.SourceManager->getSpellingLoc(End)),
      *Result.SourceManager, Result.Context->getLangOpts());
  // Insert the forward declaration back into the old namespace after moving the
  // code from old namespace to new namespace.
  // Insertion information is stored in `InsertFwdDecls` and actual
  // insertion will be performed in `onEndOfTranslationUnit`.
  // Get the (old) namespace that contains the forward declaration.
  const auto *NsDecl = Result.Nodes.getNodeAs<NamespaceDecl>("ns_decl");
  // The namespace contains the forward declaration, so it must not be empty.
  assert(!NsDecl->decls_empty());
  const auto Insertion = createInsertion(NsDecl->decls_begin()->getLocStart(),
                                         Code, *Result.SourceManager);
  InsertForwardDeclaration InsertFwd;
  InsertFwd.InsertionOffset = Insertion.getOffset();
  InsertFwd.ForwardDeclText = Insertion.getReplacementText().str();
  InsertFwdDecls[Insertion.getFilePath()].push_back(InsertFwd);
}

// Replaces a qualified symbol (in \p DeclCtx) that refers to a declaration \p
// FromDecl with the shortest qualified name possible when the reference is in
// `NewNamespace`.
void ChangeNamespaceTool::replaceQualifiedSymbolInDeclContext(
    const ast_matchers::MatchFinder::MatchResult &Result,
    const DeclContext *DeclCtx, SourceLocation Start, SourceLocation End,
    const NamedDecl *FromDecl) {
  const auto *NsDeclContext = DeclCtx->getEnclosingNamespaceContext();
  const auto *NsDecl = llvm::cast<NamespaceDecl>(NsDeclContext);
  // Calculate the name of the `NsDecl` after it is moved to new namespace.
  std::string OldNs = NsDecl->getQualifiedNameAsString();
  llvm::StringRef Postfix = OldNs;
  bool Consumed = Postfix.consume_front(OldNamespace);
  assert(Consumed && "Expect OldNS to start with OldNamespace.");
  (void)Consumed;
  const std::string NewNs = (NewNamespace + Postfix).str();

  llvm::StringRef NestedName = Lexer::getSourceText(
      CharSourceRange::getTokenRange(
          Result.SourceManager->getSpellingLoc(Start),
          Result.SourceManager->getSpellingLoc(End)),
      *Result.SourceManager, Result.Context->getLangOpts());
  // If the symbol is already fully qualified, no change needs to be make.
  if (NestedName.startswith("::"))
    return;
  std::string FromDeclName = FromDecl->getQualifiedNameAsString();
  std::string ReplaceName =
      getShortestQualifiedNameInNamespace(FromDeclName, NewNs);
  // Checks if there is any using namespace declarations that can shorten the
  // qualified name.
  for (const auto *UsingNamespace : UsingNamespaceDecls) {
    if (!isDeclVisibleAtLocation(*Result.SourceManager, UsingNamespace, DeclCtx,
                                 Start))
      continue;
    StringRef FromDeclNameRef = FromDeclName;
    if (FromDeclNameRef.consume_front(UsingNamespace->getNominatedNamespace()
                                          ->getQualifiedNameAsString())) {
      FromDeclNameRef = FromDeclNameRef.drop_front(2);
      if (FromDeclNameRef.size() < ReplaceName.size())
        ReplaceName = FromDeclNameRef;
    }
  }
  // Checks if there is any using shadow declarations that can shorten the
  // qualified name.
  bool Matched = false;
  for (const UsingDecl *Using : UsingDecls) {
    if (Matched)
      break;
    if (isDeclVisibleAtLocation(*Result.SourceManager, Using, DeclCtx, Start)) {
      for (const auto *UsingShadow : Using->shadows()) {
        const auto *TargetDecl = UsingShadow->getTargetDecl();
        if (TargetDecl == FromDecl) {
          ReplaceName = FromDecl->getNameAsString();
          Matched = true;
          break;
        }
      }
    }
  }
  // If the new nested name in the new namespace is the same as it was in the
  // old namespace, we don't create replacement.
  if (NestedName == ReplaceName)
    return;
  auto R = createReplacement(Start, End, ReplaceName, *Result.SourceManager);
  auto Err = FileToReplacements[R.getFilePath()].add(R);
  if (Err)
    llvm_unreachable(llvm::toString(std::move(Err)).c_str());
}

// Replace the [Start, End] of `Type` with the shortest qualified name when the
// `Type` is in `NewNamespace`.
void ChangeNamespaceTool::fixTypeLoc(
    const ast_matchers::MatchFinder::MatchResult &Result, SourceLocation Start,
    SourceLocation End, TypeLoc Type) {
  // FIXME: do not rename template parameter.
  if (Start.isInvalid() || End.isInvalid())
    return;
  // Types of CXXCtorInitializers do not need to be fixed.
  if (llvm::is_contained(BaseCtorInitializerTypeLocs, Type))
    return;
  // The declaration which this TypeLoc refers to.
  const auto *FromDecl = Result.Nodes.getNodeAs<NamedDecl>("from_decl");
  // `hasDeclaration` gives underlying declaration, but if the type is
  // a typedef type, we need to use the typedef type instead.
  if (auto *Typedef = Type.getType()->getAs<TypedefType>()) {
    FromDecl = Typedef->getDecl();
    auto IsInMovedNs = [&](const NamedDecl *D) {
      if (!llvm::StringRef(D->getQualifiedNameAsString())
               .startswith(OldNamespace + "::"))
        return false;
      auto ExpansionLoc =
          Result.SourceManager->getExpansionLoc(D->getLocStart());
      if (ExpansionLoc.isInvalid())
        return false;
      llvm::StringRef Filename =
          Result.SourceManager->getFilename(ExpansionLoc);
      return FilePatternRE.match(Filename);
    };
    // Don't fix the \p Type if it refers to a type alias decl in the moved
    // namespace since the alias decl will be moved along with the type
    // reference.
    if (IsInMovedNs(FromDecl))
      return;
  }

  const Decl *DeclCtx = Result.Nodes.getNodeAs<Decl>("dc");
  assert(DeclCtx && "Empty decl context.");
  replaceQualifiedSymbolInDeclContext(Result, DeclCtx->getDeclContext(), Start,
                                      End, FromDecl);
}

void ChangeNamespaceTool::fixUsingShadowDecl(
    const ast_matchers::MatchFinder::MatchResult &Result,
    const UsingDecl *UsingDeclaration) {
  SourceLocation Start = UsingDeclaration->getLocStart();
  SourceLocation End = UsingDeclaration->getLocEnd();
  if (Start.isInvalid() || End.isInvalid())
    return;

  assert(UsingDeclaration->shadow_size() > 0);
  // FIXME: it might not be always accurate to use the first using-decl.
  const NamedDecl *TargetDecl =
      UsingDeclaration->shadow_begin()->getTargetDecl();
  std::string TargetDeclName = TargetDecl->getQualifiedNameAsString();
  // FIXME: check if target_decl_name is in moved ns, which doesn't make much
  // sense. If this happens, we need to use name with the new namespace.
  // Use fully qualified name in UsingDecl for now.
  auto R = createReplacement(Start, End, "using ::" + TargetDeclName,
                             *Result.SourceManager);
  auto Err = FileToReplacements[R.getFilePath()].add(R);
  if (Err)
    llvm_unreachable(llvm::toString(std::move(Err)).c_str());
}

void ChangeNamespaceTool::fixDeclRefExpr(
    const ast_matchers::MatchFinder::MatchResult &Result,
    const DeclContext *UseContext, const NamedDecl *From,
    const DeclRefExpr *Ref) {
  SourceRange RefRange = Ref->getSourceRange();
  replaceQualifiedSymbolInDeclContext(Result, UseContext, RefRange.getBegin(),
                                      RefRange.getEnd(), From);
}

void ChangeNamespaceTool::onEndOfTranslationUnit() {
  // Move namespace blocks and insert forward declaration to old namespace.
  for (const auto &FileAndNsMoves : MoveNamespaces) {
    auto &NsMoves = FileAndNsMoves.second;
    if (NsMoves.empty())
      continue;
    const std::string &FilePath = FileAndNsMoves.first;
    auto &Replaces = FileToReplacements[FilePath];
    auto &SM = *NsMoves.begin()->SourceMgr;
    llvm::StringRef Code = SM.getBufferData(NsMoves.begin()->FID);
    auto ChangedCode = tooling::applyAllReplacements(Code, Replaces);
    if (!ChangedCode) {
      llvm::errs() << llvm::toString(ChangedCode.takeError()) << "\n";
      continue;
    }
    // Replacements on the changed code for moving namespaces and inserting
    // forward declarations to old namespaces.
    tooling::Replacements NewReplacements;
    // Cut the changed code from the old namespace and paste the code in the new
    // namespace.
    for (const auto &NsMove : NsMoves) {
      // Calculate the range of the old namespace block in the changed
      // code.
      const unsigned NewOffset = Replaces.getShiftedCodePosition(NsMove.Offset);
      const unsigned NewLength =
          Replaces.getShiftedCodePosition(NsMove.Offset + NsMove.Length) -
          NewOffset;
      tooling::Replacement Deletion(FilePath, NewOffset, NewLength, "");
      std::string MovedCode = ChangedCode->substr(NewOffset, NewLength);
      std::string MovedCodeWrappedInNewNs =
          wrapCodeInNamespace(DiffNewNamespace, MovedCode);
      // Calculate the new offset at which the code will be inserted in the
      // changed code.
      unsigned NewInsertionOffset =
          Replaces.getShiftedCodePosition(NsMove.InsertionOffset);
      tooling::Replacement Insertion(FilePath, NewInsertionOffset, 0,
                                     MovedCodeWrappedInNewNs);
      addOrMergeReplacement(Deletion, &NewReplacements);
      addOrMergeReplacement(Insertion, &NewReplacements);
    }
    // After moving namespaces, insert forward declarations back to old
    // namespaces.
    const auto &FwdDeclInsertions = InsertFwdDecls[FilePath];
    for (const auto &FwdDeclInsertion : FwdDeclInsertions) {
      unsigned NewInsertionOffset =
          Replaces.getShiftedCodePosition(FwdDeclInsertion.InsertionOffset);
      tooling::Replacement Insertion(FilePath, NewInsertionOffset, 0,
                                     FwdDeclInsertion.ForwardDeclText);
      addOrMergeReplacement(Insertion, &NewReplacements);
    }
    // Add replacements referring to the changed code to existing replacements,
    // which refers to the original code.
    Replaces = Replaces.merge(NewReplacements);
    format::FormatStyle Style =
        format::getStyle("file", FilePath, FallbackStyle);
    // Clean up old namespaces if there is nothing in it after moving.
    auto CleanReplacements =
        format::cleanupAroundReplacements(Code, Replaces, Style);
    if (!CleanReplacements) {
      llvm::errs() << llvm::toString(CleanReplacements.takeError()) << "\n";
      continue;
    }
    FileToReplacements[FilePath] = *CleanReplacements;
  }

  // Make sure we don't generate replacements for files that do not match
  // FilePattern.
  for (auto &Entry : FileToReplacements)
    if (!FilePatternRE.match(Entry.first))
      Entry.second.clear();
}

} // namespace change_namespace
} // namespace clang
