//===--- GenericEnvironment.cpp - GenericEnvironment AST ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the GenericEnvironment class.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/ProtocolConformance.h"

using namespace swift;

GenericEnvironment::GenericEnvironment(GenericSignature *signature,
                                       GenericSignatureBuilder *builder)
  : Signature(signature), Builder(builder)
{
  // Clear out the memory that holds the context types.
  std::uninitialized_fill(getContextTypes().begin(), getContextTypes().end(),
                          Type());
}

void GenericEnvironment::setOwningDeclContext(DeclContext *newOwningDC) {
  if (!OwningDC) {
    OwningDC = newOwningDC;
    return;
  }

  if (!newOwningDC || OwningDC == newOwningDC)
    return;

  // Find the least common ancestor context to be the owner.
  unsigned oldDepth = OwningDC->getSyntacticDepth();
  unsigned newDepth = newOwningDC->getSyntacticDepth();

  while (oldDepth > newDepth) {
    OwningDC = OwningDC->getParent();
    --oldDepth;
  }

  while (newDepth > oldDepth) {
    newOwningDC = newOwningDC->getParent();
    --newDepth;
  }

  while (OwningDC != newOwningDC) {
    OwningDC = OwningDC->getParent();
    newOwningDC = newOwningDC->getParent();
  }
}

void GenericEnvironment::addMapping(GenericParamKey key,
                                    Type contextType) {
  // Find the index into the parallel arrays of generic parameters and
  // context types.
  auto genericParams = Signature->getGenericParams();
  unsigned index = key.findIndexIn(genericParams);
  assert(genericParams[index] == key && "Bad generic parameter");

  // Add the mapping from the generic parameter to the context type.
  assert(getContextTypes()[index].isNull() && "Already recoded this mapping");
  getContextTypes()[index] = contextType;
}

Optional<Type> GenericEnvironment::getMappingIfPresent(
                                                    GenericParamKey key) const {
  // Find the index into the parallel arrays of generic parameters and
  // context types.
  auto genericParams = Signature->getGenericParams();
  unsigned index = key.findIndexIn(genericParams);
  assert(genericParams[index] == key && "Bad generic parameter");

  if (auto type = getContextTypes()[index])
    return type;

  return None;
}

Type GenericEnvironment::mapTypeIntoContext(GenericEnvironment *env,
                                            Type type) {
  assert(!type->hasArchetype() && "already have a contextual type");

  if (!env)
    return type.substDependentTypesWithErrorTypes();

  return env->mapTypeIntoContext(type);
}

Type
GenericEnvironment::mapTypeOutOfContext(GenericEnvironment *env,
                                        Type type) {
  assert(!type->hasTypeParameter() && "already have an interface type");

  if (!env)
    return type.substDependentTypesWithErrorTypes();

  return env->mapTypeOutOfContext(type);
}

Type GenericEnvironment::mapTypeOutOfContext(Type type) const {
  type = type.subst([&](SubstitutableType *t) -> Type {
                      return cast<ArchetypeType>(t)->getInterfaceType();
                    },
                    MakeAbstractConformanceForGenericType(),
                    SubstFlags::AllowLoweredTypes);
  assert(!type->hasArchetype() && "not fully substituted");
  return type;
}

Type GenericEnvironment::QueryInterfaceTypeSubstitutions::operator()(
                                                SubstitutableType *type) const {
  if (auto gp = type->getAs<GenericTypeParamType>()) {
    // Find the index into the parallel arrays of generic parameters and
    // context types.
    auto genericParams = self->Signature->getGenericParams();
    GenericParamKey key(gp);

    // Make sure that this generic parameter is from this environment.
    unsigned index = key.findIndexIn(genericParams);
    if (index == genericParams.size() || genericParams[index] != key)
      return Type();

    // If the context type isn't already known, lazily create it.
    Type contextType = self->getContextTypes()[index];
    if (!contextType) {
      assert(self->Builder && "Missing generic signature builder for lazy query");
      auto equivClass =
        self->Builder->resolveEquivalenceClass(
                                  type,
                                  ArchetypeResolutionKind::CompleteWellFormed);

      auto mutableSelf = const_cast<GenericEnvironment *>(self);
      contextType = equivClass->getTypeInContext(*mutableSelf->Builder,
                                                 mutableSelf);

      // FIXME: Redundant mapping from key -> index.
      if (self->getContextTypes()[index].isNull())
        mutableSelf->addMapping(key, contextType);
    }

    return contextType;
  }

  return Type();
}

Type GenericEnvironment::mapTypeIntoContext(
                                Type type,
                                LookupConformanceFn lookupConformance) const {
  assert(!type->hasOpenedExistential() &&
         "Opened existentials are special and so are you");

  Type result = type.subst(QueryInterfaceTypeSubstitutions(this),
                           lookupConformance,
                           (SubstFlags::AllowLoweredTypes|
                            SubstFlags::UseErrorType));
  assert((!result->hasTypeParameter() || result->hasError()) &&
         "not fully substituted");
  return result;

}

Type GenericEnvironment::mapTypeIntoContext(Type type) const {
  auto *sig = getGenericSignature();
  return mapTypeIntoContext(type, LookUpConformanceInSignature(*sig));
}

Type GenericEnvironment::mapTypeIntoContext(GenericTypeParamType *type) const {
  auto self = const_cast<GenericEnvironment *>(this);
  Type result = QueryInterfaceTypeSubstitutions(self)(type);
  if (!result)
    return ErrorType::get(type);
  return result;
}

GenericTypeParamType *GenericEnvironment::getSugaredType(
    GenericTypeParamType *type) const {
  for (auto *sugaredType : getGenericParams())
    if (sugaredType->isEqual(type))
      return sugaredType;

  llvm_unreachable("missing generic parameter");
}

Type GenericEnvironment::getSugaredType(Type type) const {
  if (!type->hasTypeParameter())
    return type;

  return type.transform([this](Type Ty) -> Type {
    if (auto GP = dyn_cast<GenericTypeParamType>(Ty.getPointer())) {
      return Type(getSugaredType(GP));
    }
    return Ty;
  });
}

SubstitutionList
GenericEnvironment::getForwardingSubstitutions() const {
  auto *genericSig = getGenericSignature();

  SubstitutionMap subMap = genericSig->getSubstitutionMap(
    QueryInterfaceTypeSubstitutions(this),
    MakeAbstractConformanceForGenericType());

  SmallVector<Substitution, 4> result;
  genericSig->getSubstitutions(subMap, result);
  return genericSig->getASTContext().AllocateCopy(result);
}

SubstitutionMap
GenericEnvironment::
getSubstitutionMap(TypeSubstitutionFn subs,
                   LookupConformanceFn lookupConformance) const {
  SubstitutionMap subMap(const_cast<GenericEnvironment *>(this));

  getGenericSignature()->enumeratePairedRequirements(
    [&](Type depTy, ArrayRef<Requirement> reqs) -> bool {
      auto canTy = depTy->getCanonicalType();

      // Map the interface type to a context type.
      auto contextTy = depTy.subst(QueryInterfaceTypeSubstitutions(this),
                                   MakeAbstractConformanceForGenericType());

      // Compute the replacement type.
      Type currentReplacement = contextTy.subst(subs, lookupConformance,
                                                SubstFlags::UseErrorType);

      if (auto paramTy = dyn_cast<GenericTypeParamType>(canTy))
        subMap.addSubstitution(paramTy, currentReplacement);

      // Collect the conformances.
      for (auto req: reqs) {
        assert(req.getKind() == RequirementKind::Conformance);
        auto protoType = req.getSecondType()->castTo<ProtocolType>();
        auto conformance = lookupConformance(canTy,
                                             currentReplacement,
                                             protoType);
        if (conformance) {
          assert(conformance->getConditionalRequirements().empty() &&
                 "unhandled conditional requirements");

          subMap.addConformance(canTy, *conformance);
        }
      }

      return false;
    });

  subMap.verify();
  return subMap;
}
