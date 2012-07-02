//===- Calls.h - Wrapper for all function and method calls --------*- C++ -*--//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file This file defines CallEvent and its subclasses, which represent path-
/// sensitive instances of different kinds of function and method calls
/// (C, C++, and Objective-C).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_STATICANALYZER_PATHSENSITIVE_CALL
#define LLVM_CLANG_STATICANALYZER_PATHSENSITIVE_CALL

#include "clang/Basic/SourceManager.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ObjCMessage.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState_Fwd.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"

namespace clang {
namespace ento {

enum CallEventKind {
  CE_Function,
  CE_CXXMember,
  CE_Block,
  CE_BEG_SIMPLE_CALLS = CE_Function,
  CE_END_SIMPLE_CALLS = CE_Block,
  CE_CXXConstructor,
  CE_BEG_FUNCTION_CALLS = CE_Function,
  CE_END_FUNCTION_CALLS = CE_CXXConstructor,
  CE_ObjCMessage
};

/// \brief Represents an abstract call to a function or method along a
/// particular path.
class CallEvent {
public:
  typedef CallEventKind Kind;

protected:
  ProgramStateRef State;
  const LocationContext *LCtx;
  const Kind K;

  CallEvent(ProgramStateRef state, const LocationContext *lctx, Kind k)
    : State(state), LCtx(lctx), K(k) {}
  virtual ~CallEvent() {}

  /// \brief Get the value of arbitrary expressions at this point in the path.
  SVal getSVal(const Stmt *S) const {
    return State->getSVal(S, LCtx);
  }

  typedef SmallVectorImpl<const MemRegion *> RegionList;

  /// \brief Used to specify non-argument regions that will be invalidated as a
  /// result of this call.
  virtual void addExtraInvalidatedRegions(RegionList &Regions) const {}

  typedef const ParmVarDecl * const *param_iterator;
  virtual param_iterator param_begin() const = 0;
  virtual param_iterator param_end() const = 0;

  virtual QualType getDeclaredResultType() const { return QualType(); }

public:
  /// \brief Returns the declaration of the function or method that will be
  /// called. May be null.
  virtual const Decl *getDecl() const = 0;

  /// \brief Returns the expression whose value will be the result of this call.
  /// May be null.
  virtual const Expr *getOriginExpr() const = 0;

  /// \brief Returns the number of arguments (explicit and implicit).
  ///
  /// Note that this may be greater than the number of parameters in the
  /// callee's declaration, and that it may include arguments not written in
  /// the source.
  virtual unsigned getNumArgs() const = 0;

  /// \brief Returns true if the callee is known to be from a system header.
  bool isInSystemHeader() const {
    const Decl *D = getDecl();
    if (!D)
      return false;

    SourceLocation Loc = D->getLocation();
    if (Loc.isValid()) {
      const SourceManager &SM =
        State->getStateManager().getContext().getSourceManager();
      return SM.isInSystemHeader(D->getLocation());
    }

    // Special case for implicitly-declared global operator new/delete.
    // These should be considered system functions.
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D))
      return FD->isOverloadedOperator() && FD->isImplicit() && FD->isGlobal();

    return false;
  }

  /// \brief Returns the kind of call this is.
  Kind getKind() const { return K; }

  /// \brief Returns the value of a given argument at the time of the call.
  virtual SVal getArgSVal(unsigned Index) const;

  /// \brief Returns the expression associated with a given argument.
  /// May be null if this expression does not appear in the source.
  virtual const Expr *getArgExpr(unsigned Index) const {
    return 0;
  }

  /// \brief Returns the source range for errors associated with this argument.
  /// May be invalid if the argument is not written in the source.
  // FIXME: Is it better to return an invalid range or the range of the origin
  // expression?
  virtual SourceRange getArgSourceRange(unsigned Index) const;

  /// \brief Returns the result type, adjusted for references.
  QualType getResultType() const;

  /// \brief Returns true if any of the arguments appear to represent callbacks.
  bool hasNonZeroCallbackArg() const;

  /// \brief Returns a new state with all argument regions invalidated.
  ///
  /// This accepts an alternate state in case some processing has already
  /// occurred.
  ProgramStateRef invalidateRegions(unsigned BlockCount,
                                    ProgramStateRef Orig = 0) const;

  // Iterator access to parameter types.
private:
  typedef std::const_mem_fun_t<QualType, ParmVarDecl> get_type_fun;
  
public:
  typedef llvm::mapped_iterator<param_iterator, get_type_fun>
    param_type_iterator;

  param_type_iterator param_type_begin() const {
    return llvm::map_iterator(param_begin(),
                              get_type_fun(&ParmVarDecl::getType));
  }
  param_type_iterator param_type_end() const {
    return llvm::map_iterator(param_end(), get_type_fun(&ParmVarDecl::getType));
  }

  static bool classof(const CallEvent *) { return true; }
};

/// \brief Represents a call to any sort of function that might have a
/// FunctionDecl.
class AnyFunctionCall : public CallEvent {
protected:
  AnyFunctionCall(ProgramStateRef St, const LocationContext *LCtx, Kind K)
    : CallEvent(St, LCtx, K) {}

  param_iterator param_begin() const;
  param_iterator param_end() const;

  QualType getDeclaredResultType() const;

public:
  virtual const FunctionDecl *getDecl() const = 0;

  static bool classof(const CallEvent *CA) {
    return CA->getKind() >= CE_BEG_FUNCTION_CALLS &&
           CA->getKind() <= CE_END_FUNCTION_CALLS;
  }
};

/// \brief Represents a call to a written as a CallExpr.
class SimpleCall : public AnyFunctionCall {
  const CallExpr *CE;

protected:
  SimpleCall(const CallExpr *ce, ProgramStateRef St,
             const LocationContext *LCtx, Kind K)
    : AnyFunctionCall(St, LCtx, K), CE(ce) {
  }

public:
  const CallExpr *getOriginExpr() const { return CE; }

  const FunctionDecl *getDecl() const;

  unsigned getNumArgs() const { return CE->getNumArgs(); }
  
  const Expr *getArgExpr(unsigned Index) const {
    return CE->getArg(Index);
  }

  static bool classof(const CallEvent *CA) {
    return CA->getKind() >= CE_BEG_SIMPLE_CALLS &&
           CA->getKind() <= CE_END_SIMPLE_CALLS;
  }
};

/// \brief Represents a C function or static C++ member function call.
///
/// Example: \c fun()
class FunctionCall : public SimpleCall {
public:
  FunctionCall(const CallExpr *CE, ProgramStateRef St,
               const LocationContext *LCtx)
    : SimpleCall(CE, St, LCtx, CE_Function) {}

  static bool classof(const CallEvent *CA) {
    return CA->getKind() == CE_Function;
  }
};

/// \brief Represents a non-static C++ member function call.
///
/// Example: \c obj.fun()
class CXXMemberCall : public SimpleCall {
protected:
  void addExtraInvalidatedRegions(RegionList &Regions) const;

public:
  CXXMemberCall(const CXXMemberCallExpr *CE, ProgramStateRef St,
               const LocationContext *LCtx)
    : SimpleCall(CE, St, LCtx, CE_CXXMember) {}

  const CXXMemberCallExpr *getOriginExpr() const {
    return cast<CXXMemberCallExpr>(SimpleCall::getOriginExpr());
  }

  static bool classof(const CallEvent *CA) {
    return CA->getKind() == CE_CXXMember;
  }
};

/// \brief Represents a call to a block.
///
/// Example: \c ^{ /* ... */ }()
class BlockCall : public SimpleCall {
protected:
  void addExtraInvalidatedRegions(RegionList &Regions) const;

  param_iterator param_begin() const;
  param_iterator param_end() const;

  QualType getDeclaredResultType() const;

public:
  BlockCall(const CallExpr *CE, ProgramStateRef St,
            const LocationContext *LCtx)
    : SimpleCall(CE, St, LCtx, CE_Block) {
    assert(isa<BlockDataRegion>(getSVal(CE->getCallee()).getAsRegion()));
  }

  static bool classof(const CallEvent *CA) {
    return CA->getKind() == CE_Block;
  }

private:
  const BlockDataRegion *getBlockRegion() const;
};

/// \brief Represents a call to a C++ constructor.
///
/// Example: \c T(1)
class CXXConstructorCall : public AnyFunctionCall {
  const CXXConstructExpr *CE;
  const MemRegion *Target;

protected:
  void addExtraInvalidatedRegions(RegionList &Regions) const;

public:
  CXXConstructorCall(const CXXConstructExpr *ce, ProgramStateRef St,
                     const LocationContext *LCtx)
    : AnyFunctionCall(St, LCtx, CE_CXXConstructor), CE(ce), Target(0) {}
  CXXConstructorCall(const CXXConstructExpr *ce, const MemRegion *target,
                     ProgramStateRef St, const LocationContext *LCtx)
    : AnyFunctionCall(St, LCtx, CE_CXXConstructor), CE(ce), Target(target) {}

  const CXXConstructExpr *getOriginExpr() const { return CE; }

  const CXXConstructorDecl *getDecl() const {
    return CE->getConstructor();
  }

  unsigned getNumArgs() const { return CE->getNumArgs(); }

  const Expr *getArgExpr(unsigned Index) const {
    return CE->getArg(Index);
  }

  static bool classof(const CallEvent *CA) {
    return CA->getKind() == CE_CXXConstructor;
  }
};

/// \brief Represents any expression that causes an Objective-C message send.
//
// This class is mostly passthrough to ObjCMessage, because /that/ class is the
// adapter for the different kinds of Objective-C messages in the system. The
// difference here is that like other CallActions this refers to a specific
// (path-sensitive) message send, while ObjCMessage is simply a wrapper for the
// various (path-insensitive) expressions that are implemented using messages.
class ObjCMessageInvocation : public CallEvent {
  ObjCMessage Msg;

protected:
  void addExtraInvalidatedRegions(RegionList &Regions) const;

  param_iterator param_begin() const;
  param_iterator param_end() const;

  QualType getDeclaredResultType() const;

public:
  ObjCMessageInvocation(const ObjCMessage &msg, ProgramStateRef St,
                        const LocationContext *LCtx)
    : CallEvent(St, LCtx, CE_ObjCMessage), Msg(msg) {}

  Selector getSelector() const { return Msg.getSelector(); }
  bool isInstanceMessage() const { return Msg.isInstanceMessage(); }
  const ObjCMethodDecl *getDecl() const { return Msg.getMethodDecl(); }
  unsigned getNumArgs() const { return Msg.getNumArgs(); }
  const Expr *getArgExpr(unsigned Index) const { return Msg.getArgExpr(Index); }

  // FIXME: for emitting warnings and such this may not be the best idea.
  const Expr *getOriginExpr() const { return Msg.getMessageExpr(); }

  SVal getReceiverSVal() const;

  SourceRange getReceiverSourceRange() const {
    return Msg.getReceiverSourceRange();
  }

  const ObjCInterfaceDecl *getReceiverInterface() const {
    return Msg.getReceiverInterface();
  }

  static bool classof(const CallEvent *CA) {
    return CA->getKind() == CE_ObjCMessage;
  }
};

} // end namespace ento
} // end namespace clang

#endif