/*
 * Copyright 2011-2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "slang_rs_export_foreach.h"

#include <string>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/TypeLoc.h"

#include "llvm/DerivedTypes.h"
#include "llvm/Target/TargetData.h"

#include "slang_assert.h"
#include "slang_rs_context.h"
#include "slang_rs_export_type.h"
#include "slang_version.h"

namespace slang {

namespace {

static void ReportNameError(clang::DiagnosticsEngine *DiagEngine,
                            clang::ParmVarDecl const *PVD) {
  slangAssert(DiagEngine && PVD);
  const clang::SourceManager &SM = DiagEngine->getSourceManager();

  DiagEngine->Report(
    clang::FullSourceLoc(PVD->getLocation(), SM),
    DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                "Duplicate parameter entry "
                                "(by position/name): '%0'"))
    << PVD->getName();
  return;
}

}  // namespace

// This function takes care of additional validation and construction of
// parameters related to forEach_* reflection.
bool RSExportForEach::validateAndConstructParams(
    RSContext *Context, const clang::FunctionDecl *FD) {
  slangAssert(Context && FD);
  bool valid = true;
  clang::ASTContext &C = Context->getASTContext();
  clang::DiagnosticsEngine *DiagEngine = Context->getDiagnostics();

  numParams = FD->getNumParams();
  slangAssert(numParams > 0);

  if (Context->getTargetAPI() < SLANG_JB_TARGET_API) {
    if (!isRootRSFunc(FD)) {
      DiagEngine->Report(
        clang::FullSourceLoc(FD->getLocation(), DiagEngine->getSourceManager()),
        DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                    "Non-root compute kernel %0() is "
                                    "not supported in SDK levels %1-%2"))
        << FD->getName()
        << SLANG_MINIMUM_TARGET_API
        << (SLANG_JB_TARGET_API - 1);
      return false;
    }
  }

  // Compute kernel functions are required to return a void type for now
  if (FD->getResultType().getCanonicalType() != C.VoidTy) {
    DiagEngine->Report(
      clang::FullSourceLoc(FD->getLocation(), DiagEngine->getSourceManager()),
      DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                  "Compute kernel %0() is required to return a "
                                  "void type")) << FD->getName();
    valid = false;
  }

  // Validate remaining parameter types
  // TODO(all): Add support for LOD/face when we have them

  size_t i = 0;
  const clang::ParmVarDecl *PVD = FD->getParamDecl(i);
  clang::QualType QT = PVD->getType().getCanonicalType();

  // Check for const T1 *in
  if (QT->isPointerType() && QT->getPointeeType().isConstQualified()) {
    mIn = PVD;
    i++;  // advance parameter pointer
  }

  // Check for T2 *out
  if (i < numParams) {
    PVD = FD->getParamDecl(i);
    QT = PVD->getType().getCanonicalType();
    if (QT->isPointerType() && !QT->getPointeeType().isConstQualified()) {
      mOut = PVD;
      i++;  // advance parameter pointer
    }
  }

  if (!mIn && !mOut) {
    DiagEngine->Report(
      clang::FullSourceLoc(FD->getLocation(),
                           DiagEngine->getSourceManager()),
      DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                  "Compute kernel %0() must have at least one "
                                  "parameter for in or out")) << FD->getName();
    valid = false;
  }

  // Check for T3 *usrData
  if (i < numParams) {
    PVD = FD->getParamDecl(i);
    QT = PVD->getType().getCanonicalType();
    if (QT->isPointerType() && QT->getPointeeType().isConstQualified()) {
      mUsrData = PVD;
      i++;  // advance parameter pointer
    }
  }

  while (i < numParams) {
    PVD = FD->getParamDecl(i);
    QT = PVD->getType().getCanonicalType();

    if (QT.getUnqualifiedType() != C.UnsignedIntTy) {
      DiagEngine->Report(
        clang::FullSourceLoc(PVD->getLocation(),
                             DiagEngine->getSourceManager()),
        DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                    "Unexpected kernel %0() parameter '%1' "
                                    "of type '%2'"))
        << FD->getName() << PVD->getName() << PVD->getType().getAsString();
      valid = false;
    } else {
      llvm::StringRef ParamName = PVD->getName();
      if (ParamName.equals("x")) {
        if (mX) {
          ReportNameError(DiagEngine, PVD);
          valid = false;
        } else if (mY) {
          // Can't go back to X after skipping Y
          ReportNameError(DiagEngine, PVD);
          valid = false;
        } else {
          mX = PVD;
        }
      } else if (ParamName.equals("y")) {
        if (mY) {
          ReportNameError(DiagEngine, PVD);
          valid = false;
        } else {
          mY = PVD;
        }
      } else {
        if (!mX && !mY) {
          mX = PVD;
        } else if (!mY) {
          mY = PVD;
        } else {
          DiagEngine->Report(
            clang::FullSourceLoc(PVD->getLocation(),
                                 DiagEngine->getSourceManager()),
            DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                        "Unexpected kernel %0() parameter '%1' "
                                        "of type '%2'"))
            << FD->getName() << PVD->getName() << PVD->getType().getAsString();
          valid = false;
        }
      }
    }

    i++;
  }

  mSignatureMetadata = 0;
  if (valid) {
    // Set up the bitwise metadata encoding for runtime argument passing.
    mSignatureMetadata |= (mIn ?       0x01 : 0);
    mSignatureMetadata |= (mOut ?      0x02 : 0);
    mSignatureMetadata |= (mUsrData ?  0x04 : 0);
    mSignatureMetadata |= (mX ?        0x08 : 0);
    mSignatureMetadata |= (mY ?        0x10 : 0);
  }

  if (Context->getTargetAPI() < SLANG_ICS_TARGET_API) {
    // APIs before ICS cannot skip between parameters. It is ok, however, for
    // them to omit further parameters (i.e. skipping X is ok if you skip Y).
    if (mSignatureMetadata != 0x1f &&  // In, Out, UsrData, X, Y
        mSignatureMetadata != 0x0f &&  // In, Out, UsrData, X
        mSignatureMetadata != 0x07 &&  // In, Out, UsrData
        mSignatureMetadata != 0x03 &&  // In, Out
        mSignatureMetadata != 0x01) {  // In
      DiagEngine->Report(
        clang::FullSourceLoc(FD->getLocation(),
                             DiagEngine->getSourceManager()),
        DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                    "Compute kernel %0() targeting SDK levels "
                                    "%1-%2 may not skip parameters"))
        << FD->getName() << SLANG_MINIMUM_TARGET_API
        << (SLANG_ICS_TARGET_API - 1);
      valid = false;
    }
  }

  return valid;
}

RSExportForEach *RSExportForEach::Create(RSContext *Context,
                                         const clang::FunctionDecl *FD) {
  slangAssert(Context && FD);
  llvm::StringRef Name = FD->getName();
  RSExportForEach *FE;

  slangAssert(!Name.empty() && "Function must have a name");

  FE = new RSExportForEach(Context, Name);

  if (!FE->validateAndConstructParams(Context, FD)) {
    return NULL;
  }

  clang::ASTContext &Ctx = Context->getASTContext();

  std::string Id(DUMMY_RS_TYPE_NAME_PREFIX"helper_foreach_param:");
  Id.append(FE->getName()).append(DUMMY_RS_TYPE_NAME_POSTFIX);

  // Extract the usrData parameter (if we have one)
  if (FE->mUsrData) {
    const clang::ParmVarDecl *PVD = FE->mUsrData;
    clang::QualType QT = PVD->getType().getCanonicalType();
    slangAssert(QT->isPointerType() &&
                QT->getPointeeType().isConstQualified());

    const clang::ASTContext &C = Context->getASTContext();
    if (QT->getPointeeType().getCanonicalType().getUnqualifiedType() ==
        C.VoidTy) {
      // In the case of using const void*, we can't reflect an appopriate
      // Java type, so we fall back to just reflecting the ain/aout parameters
      FE->mUsrData = NULL;
    } else {
      clang::RecordDecl *RD =
          clang::RecordDecl::Create(Ctx, clang::TTK_Struct,
                                    Ctx.getTranslationUnitDecl(),
                                    clang::SourceLocation(),
                                    clang::SourceLocation(),
                                    &Ctx.Idents.get(Id));

      clang::FieldDecl *FD =
          clang::FieldDecl::Create(Ctx,
                                   RD,
                                   clang::SourceLocation(),
                                   clang::SourceLocation(),
                                   PVD->getIdentifier(),
                                   QT->getPointeeType(),
                                   NULL,
                                   /* BitWidth = */ NULL,
                                   /* Mutable = */ false,
                                   /* HasInit = */ clang::ICIS_NoInit);
      RD->addDecl(FD);
      RD->completeDefinition();

      // Create an export type iff we have a valid usrData type
      clang::QualType T = Ctx.getTagDeclType(RD);
      slangAssert(!T.isNull());

      RSExportType *ET = RSExportType::Create(Context, T.getTypePtr());

      if (ET == NULL) {
        fprintf(stderr, "Failed to export the function %s. There's at least "
                        "one parameter whose type is not supported by the "
                        "reflection\n", FE->getName().c_str());
        return NULL;
      }

      slangAssert((ET->getClass() == RSExportType::ExportClassRecord) &&
                  "Parameter packet must be a record");

      FE->mParamPacketType = static_cast<RSExportRecordType *>(ET);
    }
  }

  if (FE->mIn) {
    const clang::Type *T = FE->mIn->getType().getCanonicalType().getTypePtr();
    FE->mInType = RSExportType::Create(Context, T);
  }

  if (FE->mOut) {
    const clang::Type *T = FE->mOut->getType().getCanonicalType().getTypePtr();
    FE->mOutType = RSExportType::Create(Context, T);
  }

  return FE;
}

RSExportForEach *RSExportForEach::CreateDummyRoot(RSContext *Context) {
  slangAssert(Context);
  llvm::StringRef Name = "root";
  RSExportForEach *FE = new RSExportForEach(Context, Name);
  FE->mDummyRoot = true;
  return FE;
}

bool RSExportForEach::isGraphicsRootRSFunc(int targetAPI,
                                           const clang::FunctionDecl *FD) {
  if (!isRootRSFunc(FD)) {
    return false;
  }

  if (FD->getNumParams() == 0) {
    // Graphics root function
    return true;
  }

  // Check for legacy graphics root function (with single parameter).
  if ((targetAPI < SLANG_ICS_TARGET_API) && (FD->getNumParams() == 1)) {
    const clang::QualType &IntType = FD->getASTContext().IntTy;
    if (FD->getResultType().getCanonicalType() == IntType) {
      return true;
    }
  }

  return false;
}

bool RSExportForEach::isRSForEachFunc(int targetAPI,
    const clang::FunctionDecl *FD) {
  if (isGraphicsRootRSFunc(targetAPI, FD)) {
    return false;
  }

  // Check if first parameter is a pointer (which is required for ForEach).
  unsigned int numParams = FD->getNumParams();

  if (numParams > 0) {
    const clang::ParmVarDecl *PVD = FD->getParamDecl(0);
    clang::QualType QT = PVD->getType().getCanonicalType();

    if (QT->isPointerType()) {
      return true;
    }

    // Any non-graphics root() is automatically a ForEach candidate.
    // At this point, however, we know that it is not going to be a valid
    // compute root() function (due to not having a pointer parameter). We
    // still want to return true here, so that we can issue appropriate
    // diagnostics.
    if (isRootRSFunc(FD)) {
      return true;
    }
  }

  return false;
}

bool
RSExportForEach::validateSpecialFuncDecl(int targetAPI,
                                         clang::DiagnosticsEngine *DiagEngine,
                                         clang::FunctionDecl const *FD) {
  slangAssert(DiagEngine && FD);
  bool valid = true;
  const clang::ASTContext &C = FD->getASTContext();
  const clang::QualType &IntType = FD->getASTContext().IntTy;

  if (isGraphicsRootRSFunc(targetAPI, FD)) {
    if ((targetAPI < SLANG_ICS_TARGET_API) && (FD->getNumParams() == 1)) {
      // Legacy graphics root function
      const clang::ParmVarDecl *PVD = FD->getParamDecl(0);
      clang::QualType QT = PVD->getType().getCanonicalType();
      if (QT != IntType) {
        DiagEngine->Report(
          clang::FullSourceLoc(PVD->getLocation(),
                               DiagEngine->getSourceManager()),
          DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                      "invalid parameter type for legacy "
                                      "graphics root() function: %0"))
          << PVD->getType();
        valid = false;
      }
    }

    // Graphics root function, so verify that it returns an int
    if (FD->getResultType().getCanonicalType() != IntType) {
      DiagEngine->Report(
        clang::FullSourceLoc(FD->getLocation(),
                             DiagEngine->getSourceManager()),
        DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                    "root() is required to return "
                                    "an int for graphics usage"));
      valid = false;
    }
  } else if (isInitRSFunc(FD) || isDtorRSFunc(FD)) {
    if (FD->getNumParams() != 0) {
      DiagEngine->Report(
          clang::FullSourceLoc(FD->getLocation(),
                               DiagEngine->getSourceManager()),
          DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                      "%0(void) is required to have no "
                                      "parameters")) << FD->getName();
      valid = false;
    }

    if (FD->getResultType().getCanonicalType() != C.VoidTy) {
      DiagEngine->Report(
          clang::FullSourceLoc(FD->getLocation(),
                               DiagEngine->getSourceManager()),
          DiagEngine->getCustomDiagID(clang::DiagnosticsEngine::Error,
                                      "%0(void) is required to have a void "
                                      "return type")) << FD->getName();
      valid = false;
    }
  } else {
    slangAssert(false && "must be called on root, init or .rs.dtor function!");
  }

  return valid;
}

}  // namespace slang
