#pragma once

//
// Copyright (c) rev.ng Labs Srl. See LICENSE.md for details.
//

#include "llvm/ADT/SmallString.h"

#include "revng/Model/ForwardDecls.h"

#include "revng-c/Support/TokenDefinitions.h"

namespace ArtificialTypes {
constexpr const char *const RetStructPrefix = "artificial_struct_";
constexpr const char *const ArrayWrapperPrefix = "artificial_wrapper_";

constexpr const char *const RetFieldPrefix = "field_";
constexpr const char *const ArrayWrapperFieldName = "the_array";
} // namespace ArtificialTypes

/// Print a string containing the C Type name of \a QT and a
/// (possibly empty) \a InstanceName .
extern tokenDefinition::types::TypeString
getNamedCInstance(const model::QualifiedType &QT, llvm::StringRef InstanceName);

inline tokenDefinition::types::TypeString
getTypeName(const model::QualifiedType &QT) {
  return getNamedCInstance(QT, "");
}

/// Return the name of the array wrapper that wraps \a QT (QT must be
/// an array).
extern tokenDefinition::types::TypeString
getArrayWrapper(const model::QualifiedType &QT);

/// Return the name of the type returned by \a F
/// \note If F returns more than one value, the name of the wrapping struct
/// will be returned.
extern tokenDefinition::types::TypeString
getReturnTypeName(const model::RawFunctionType &F);

/// Return the name of the array wrapper that wraps \a QT (QT must be
/// an array).
/// \note If F returns an array, the name of the wrapping struct will be
/// returned.
extern tokenDefinition::types::TypeString
getReturnTypeName(const model::CABIFunctionType &F);

/// Return the name of the \a Index -th field of the struct returned
/// by \a F.
/// \note F must be returning more than one value, otherwise
/// there is no wrapping struct.
extern tokenDefinition::types::TypeString
getReturnField(const model::RawFunctionType &F, size_t Index);

/// Print the function prototype (without any trailing ';') of \a FT
///        using \a FunctionName as the function's name. If the return value
///        or any of the arguments needs a wrapper, print it with the
///        corresponding wrapper type. The definition of such wrappers
///        should have already been printed before this function is called.
extern void printFunctionPrototype(const model::Type &FT,
                                   const model::Function &Function,
                                   llvm::raw_ostream &Header,
                                   const model::Binary &Model,
                                   bool Definition);
extern void printFunctionPrototype(const model::Type &FT,
                                   const model::DynamicFunction &Function,
                                   llvm::raw_ostream &Header,
                                   const model::Binary &Model,
                                   bool Definition);
extern void printFunctionTypeDeclaration(const model::Type &FT,
                                         llvm::StringRef TypeName,
                                         llvm::raw_ostream &Header,
                                         const model::Binary &Model);

extern std::string getArgumentLocationDefinition(llvm::StringRef ArgumentName,
                                                 const model::Function &F);
extern std::string getArgumentLocationReference(llvm::StringRef ArgumentName,
                                                const model::Function &F);
extern std::string getVariableLocationDefinition(llvm::StringRef VariableName,
                                                 const model::Function &F);
extern std::string getVariableLocationReference(llvm::StringRef VariableName,
                                                const model::Function &F);
