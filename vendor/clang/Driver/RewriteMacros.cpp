//===--- RewriteMacros.cpp - Rewrite macros into their expansions ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This code rewrites macro invocations into their expansions.  This gives you
// a macro expanded file that retains comments and #includes.
//
//===----------------------------------------------------------------------===//

#include "clang.h"
#include "clang/Rewrite/Rewriter.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/Support/Streams.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/System/Path.h"
#include "llvm/ADT/OwningPtr.h"
using namespace clang;

/// isSameToken - Return true if the two specified tokens start have the same
/// content.
static bool isSameToken(Token &RawTok, Token &PPTok) {
  // If two tokens have the same kind and the same identifier info, they are
  // obviously the same.
  if (PPTok.getKind() == RawTok.getKind() &&
      PPTok.getIdentifierInfo() == RawTok.getIdentifierInfo())
    return true;
  
  // Otherwise, if they are different but have the same identifier info, they
  // are also considered to be the same.  This allows keywords and raw lexed
  // identifiers with the same name to be treated the same.
  if (PPTok.getIdentifierInfo() &&
      PPTok.getIdentifierInfo() == RawTok.getIdentifierInfo())
    return true;
  
  return false;
}


/// GetNextRawTok - Return the next raw token in the stream, skipping over
/// comments if ReturnComment is false.
static const Token &GetNextRawTok(const std::vector<Token> &RawTokens,
                                  unsigned &CurTok, bool ReturnComment) {
  assert(CurTok < RawTokens.size() && "Overran eof!");
  
  // If the client doesn't want comments and we have one, skip it.
  if (!ReturnComment && RawTokens[CurTok].is(tok::comment))
    ++CurTok;
  
  return RawTokens[CurTok++];
}


/// LexRawTokensFromMainFile - Lets all the raw tokens from the main file into
/// the specified vector.
static void LexRawTokensFromMainFile(Preprocessor &PP,
                                     std::vector<Token> &RawTokens) {
  SourceManager &SM = PP.getSourceManager();
  
  // Create a lexer to lex all the tokens of the main file in raw mode.  Even
  // though it is in raw mode, it will not return comments.
  Lexer RawLex(SM.getMainFileID(), SM, PP.getLangOptions());

  // Switch on comment lexing because we really do want them.
  RawLex.SetCommentRetentionState(true);
  
  Token RawTok;
  do {
    RawLex.LexFromRawLexer(RawTok);
    
    // If we have an identifier with no identifier info for our raw token, look
    // up the indentifier info.  This is important for equality comparison of
    // identifier tokens.
    if (RawTok.is(tok::identifier) && !RawTok.getIdentifierInfo())
      RawTok.setIdentifierInfo(PP.LookUpIdentifierInfo(RawTok));
    
    RawTokens.push_back(RawTok);
  } while (RawTok.isNot(tok::eof));
}


/// RewriteMacrosInInput - Implement -rewrite-macros mode.
void clang::RewriteMacrosInInput(Preprocessor &PP,const std::string &InFileName,
                                 const std::string &OutFileName) {
  SourceManager &SM = PP.getSourceManager();
  
  Rewriter Rewrite;
  Rewrite.setSourceMgr(SM);
  RewriteBuffer &RB = Rewrite.getEditBuffer(SM.getMainFileID());

  std::vector<Token> RawTokens;
  LexRawTokensFromMainFile(PP, RawTokens);
  unsigned CurRawTok = 0;
  Token RawTok = GetNextRawTok(RawTokens, CurRawTok, false);

  
  // Get the first preprocessing token.
  PP.EnterMainSourceFile();
  Token PPTok;
  PP.Lex(PPTok);
  
  // Preprocess the input file in parallel with raw lexing the main file. Ignore
  // all tokens that are preprocessed from a file other than the main file (e.g.
  // a header).  If we see tokens that are in the preprocessed file but not the
  // lexed file, we have a macro expansion.  If we see tokens in the lexed file
  // that aren't in the preprocessed view, we have macros that expand to no
  // tokens, or macro arguments etc.
  while (RawTok.isNot(tok::eof) || PPTok.isNot(tok::eof)) {
    SourceLocation PPLoc = SM.getInstantiationLoc(PPTok.getLocation());

    // If PPTok is from a different source file, ignore it.
    if (!SM.isFromMainFile(PPLoc)) {
      PP.Lex(PPTok);
      continue;
    }
    
    // If the raw file hits a preprocessor directive, they will be extra tokens
    // in the raw file that don't exist in the preprocsesed file.  However, we
    // choose to preserve them in the output file and otherwise handle them
    // specially.
    if (RawTok.is(tok::hash) && RawTok.isAtStartOfLine()) {
      // If this is a #warning directive or #pragma mark (GNU extensions),
      // comment the line out.
      if (RawTokens[CurRawTok].is(tok::identifier)) {
        const IdentifierInfo *II = RawTokens[CurRawTok].getIdentifierInfo();
        if (!strcmp(II->getName(), "warning")) {
          // Comment out #warning.
          RB.InsertTextAfter(SM.getFileOffset(RawTok.getLocation()), "//", 2);
        } else if (!strcmp(II->getName(), "pragma") &&
                   RawTokens[CurRawTok+1].is(tok::identifier) &&
                  !strcmp(RawTokens[CurRawTok+1].getIdentifierInfo()->getName(),
                          "mark")){
          // Comment out #pragma mark.
          RB.InsertTextAfter(SM.getFileOffset(RawTok.getLocation()), "//", 2);
        }
      }
      
      // Otherwise, if this is a #include or some other directive, just leave it
      // in the file by skipping over the line.
      RawTok = GetNextRawTok(RawTokens, CurRawTok, false);
      while (!RawTok.isAtStartOfLine() && RawTok.isNot(tok::eof))
        RawTok = GetNextRawTok(RawTokens, CurRawTok, false);
      continue;
    }
    
    // Okay, both tokens are from the same file.  Get their offsets from the
    // start of the file.
    unsigned PPOffs = SM.getFileOffset(PPLoc);
    unsigned RawOffs = SM.getFileOffset(RawTok.getLocation());

    // If the offsets are the same and the token kind is the same, ignore them.
    if (PPOffs == RawOffs && isSameToken(RawTok, PPTok)) {
      RawTok = GetNextRawTok(RawTokens, CurRawTok, false);
      PP.Lex(PPTok);
      continue;
    }

    // If the PP token is farther along than the raw token, something was
    // deleted.  Comment out the raw token.
    if (RawOffs <= PPOffs) {
      // Comment out a whole run of tokens instead of bracketing each one with
      // comments.  Add a leading space if RawTok didn't have one.
      bool HasSpace = RawTok.hasLeadingSpace();
      RB.InsertTextAfter(RawOffs, " /*"+HasSpace, 2+!HasSpace);
      unsigned EndPos;

      do {
        EndPos = RawOffs+RawTok.getLength();

        RawTok = GetNextRawTok(RawTokens, CurRawTok, true);
        RawOffs = SM.getFileOffset(RawTok.getLocation());
        
        if (RawTok.is(tok::comment)) {
          // Skip past the comment.
          RawTok = GetNextRawTok(RawTokens, CurRawTok, false);
          break;
        }
        
      } while (RawOffs <= PPOffs && !RawTok.isAtStartOfLine() &&
               (PPOffs != RawOffs || !isSameToken(RawTok, PPTok)));

      RB.InsertTextBefore(EndPos, "*/", 2);
      continue;
    }
    
    // Otherwise, there was a replacement an expansion.  Insert the new token
    // in the output buffer.  Insert the whole run of new tokens at once to get
    // them in the right order.
    unsigned InsertPos = PPOffs;
    std::string Expansion;
    while (PPOffs < RawOffs) {
      Expansion += ' ' + PP.getSpelling(PPTok);
      PP.Lex(PPTok);
      PPLoc = SM.getInstantiationLoc(PPTok.getLocation());
      PPOffs = SM.getFileOffset(PPLoc);
    }
    Expansion += ' ';
    RB.InsertTextBefore(InsertPos, &Expansion[0], Expansion.size());
  }
  
  // Create the output file.
  llvm::OwningPtr<llvm::raw_ostream> OwnedStream;
  llvm::raw_ostream *OutFile;
  if (OutFileName == "-") {
    OutFile = &llvm::outs();
  } else if (!OutFileName.empty()) {
    std::string Err;
    OutFile = new llvm::raw_fd_ostream(OutFileName.c_str(), false, Err);
    OwnedStream.reset(OutFile);
  } else if (InFileName == "-") {
    OutFile = &llvm::outs();
  } else {
    llvm::sys::Path Path(InFileName);
    Path.eraseSuffix();
    Path.appendSuffix("cpp");
    std::string Err;
    OutFile = new llvm::raw_fd_ostream(Path.toString().c_str(), false, Err);
    OwnedStream.reset(OutFile);
  }

  // Get the buffer corresponding to MainFileID.  If we haven't changed it, then
  // we are done.
  if (const RewriteBuffer *RewriteBuf = 
      Rewrite.getRewriteBufferFor(SM.getMainFileID())) {
    //printf("Changed:\n");
    *OutFile << std::string(RewriteBuf->begin(), RewriteBuf->end());
  } else {
    fprintf(stderr, "No changes\n");
  }
  OutFile->flush();
}