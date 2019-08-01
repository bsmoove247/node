// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ast/scopes.h"

#include <set>

#include "src/ast/ast.h"
#include "src/base/optional.h"
#include "src/builtins/accessors.h"
#include "src/common/message-template.h"
#include "src/init/bootstrapper.h"
#include "src/logging/counters.h"
#include "src/objects/module-inl.h"
#include "src/objects/objects-inl.h"
#include "src/objects/scope-info.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parser.h"
#include "src/parsing/preparse-data.h"
#include "src/zone/zone-list-inl.h"

namespace v8 {
namespace internal {

// ----------------------------------------------------------------------------
// Implementation of LocalsMap
//
// Note: We are storing the handle locations as key values in the hash map.
//       When inserting a new variable via Declare(), we rely on the fact that
//       the handle location remains alive for the duration of that variable
//       use. Because a Variable holding a handle with the same location exists
//       this is ensured.

VariableMap::VariableMap(Zone* zone)
    : ZoneHashMap(8, ZoneAllocationPolicy(zone)) {}

Variable* VariableMap::Declare(Zone* zone, Scope* scope,
                               const AstRawString* name, VariableMode mode,
                               VariableKind kind,
                               InitializationFlag initialization_flag,
                               MaybeAssignedFlag maybe_assigned_flag,
                               bool* was_added) {
  // AstRawStrings are unambiguous, i.e., the same string is always represented
  // by the same AstRawString*.
  // FIXME(marja): fix the type of Lookup.
  Entry* p =
      ZoneHashMap::LookupOrInsert(const_cast<AstRawString*>(name), name->Hash(),
                                  ZoneAllocationPolicy(zone));
  *was_added = p->value == nullptr;
  if (*was_added) {
    // The variable has not been declared yet -> insert it.
    DCHECK_EQ(name, p->key);
    Variable* variable = new (zone) Variable(
        scope, name, mode, kind, initialization_flag, maybe_assigned_flag);
    p->value = variable;
  }
  return reinterpret_cast<Variable*>(p->value);
}

void VariableMap::Remove(Variable* var) {
  const AstRawString* name = var->raw_name();
  ZoneHashMap::Remove(const_cast<AstRawString*>(name), name->Hash());
}

void VariableMap::Add(Zone* zone, Variable* var) {
  const AstRawString* name = var->raw_name();
  Entry* p =
      ZoneHashMap::LookupOrInsert(const_cast<AstRawString*>(name), name->Hash(),
                                  ZoneAllocationPolicy(zone));
  DCHECK_NULL(p->value);
  DCHECK_EQ(name, p->key);
  p->value = var;
}

Variable* VariableMap::Lookup(const AstRawString* name) {
  Entry* p = ZoneHashMap::Lookup(const_cast<AstRawString*>(name), name->Hash());
  if (p != nullptr) {
    DCHECK(reinterpret_cast<const AstRawString*>(p->key) == name);
    DCHECK_NOT_NULL(p->value);
    return reinterpret_cast<Variable*>(p->value);
  }
  return nullptr;
}

// ----------------------------------------------------------------------------
// Implementation of Scope

Scope::Scope(Zone* zone)
    : zone_(zone),
      outer_scope_(nullptr),
      variables_(zone),
      scope_type_(SCRIPT_SCOPE) {
  SetDefaults();
}

Scope::Scope(Zone* zone, Scope* outer_scope, ScopeType scope_type)
    : zone_(zone),
      outer_scope_(outer_scope),
      variables_(zone),
      scope_type_(scope_type) {
  DCHECK_NE(SCRIPT_SCOPE, scope_type);
  SetDefaults();
  set_language_mode(outer_scope->language_mode());
  outer_scope_->AddInnerScope(this);
}

DeclarationScope::DeclarationScope(Zone* zone,
                                   AstValueFactory* ast_value_factory)
    : Scope(zone), function_kind_(kNormalFunction), params_(4, zone) {
  DCHECK_EQ(scope_type_, SCRIPT_SCOPE);
  SetDefaults();
  receiver_ = DeclareDynamicGlobal(ast_value_factory->this_string(),
                                   THIS_VARIABLE, this);
}

DeclarationScope::DeclarationScope(Zone* zone, Scope* outer_scope,
                                   ScopeType scope_type,
                                   FunctionKind function_kind)
    : Scope(zone, outer_scope, scope_type),
      function_kind_(function_kind),
      params_(4, zone) {
  DCHECK_NE(scope_type, SCRIPT_SCOPE);
  SetDefaults();
}

ModuleScope::ModuleScope(DeclarationScope* script_scope,
                         AstValueFactory* avfactory)
    : DeclarationScope(avfactory->zone(), script_scope, MODULE_SCOPE, kModule),
      module_descriptor_(new (avfactory->zone())
                             SourceTextModuleDescriptor(avfactory->zone())) {
  set_language_mode(LanguageMode::kStrict);
  DeclareThis(avfactory);
}

ModuleScope::ModuleScope(Isolate* isolate, Handle<ScopeInfo> scope_info,
                         AstValueFactory* avfactory)
    : DeclarationScope(avfactory->zone(), MODULE_SCOPE, scope_info),
      module_descriptor_(nullptr) {
  set_language_mode(LanguageMode::kStrict);
}

ClassScope::ClassScope(Zone* zone, Scope* outer_scope)
    : Scope(zone, outer_scope, CLASS_SCOPE) {
  set_language_mode(LanguageMode::kStrict);
}

ClassScope::ClassScope(Zone* zone, AstValueFactory* ast_value_factory,
                       Handle<ScopeInfo> scope_info)
    : Scope(zone, CLASS_SCOPE, scope_info) {
  set_language_mode(LanguageMode::kStrict);
  if (scope_info->HasClassBrand()) {
    Variable* brand =
        LookupInScopeInfo(ast_value_factory->dot_brand_string(), this);
    DCHECK_NOT_NULL(brand);
    EnsureRareData()->brand = brand;
  }
}

Scope::Scope(Zone* zone, ScopeType scope_type, Handle<ScopeInfo> scope_info)
    : zone_(zone),
      outer_scope_(nullptr),
      variables_(zone),
      scope_info_(scope_info),
      scope_type_(scope_type) {
  DCHECK(!scope_info.is_null());
  SetDefaults();
#ifdef DEBUG
  already_resolved_ = true;
#endif
  if (scope_info->CallsSloppyEval()) scope_calls_eval_ = true;
  set_language_mode(scope_info->language_mode());
  num_heap_slots_ = scope_info->ContextLength();
  DCHECK_LE(Context::MIN_CONTEXT_SLOTS, num_heap_slots_);
  // We don't really need to use the preparsed scope data; this is just to
  // shorten the recursion in SetMustUsePreparseData.
  must_use_preparsed_scope_data_ = true;
}

DeclarationScope::DeclarationScope(Zone* zone, ScopeType scope_type,
                                   Handle<ScopeInfo> scope_info)
    : Scope(zone, scope_type, scope_info),
      function_kind_(scope_info->function_kind()),
      params_(0, zone) {
  DCHECK_NE(scope_type, SCRIPT_SCOPE);
  SetDefaults();
}

Scope::Scope(Zone* zone, const AstRawString* catch_variable_name,
             MaybeAssignedFlag maybe_assigned, Handle<ScopeInfo> scope_info)
    : zone_(zone),
      outer_scope_(nullptr),
      variables_(zone),
      scope_info_(scope_info),
      scope_type_(CATCH_SCOPE) {
  SetDefaults();
#ifdef DEBUG
  already_resolved_ = true;
#endif
  // Cache the catch variable, even though it's also available via the
  // scope_info, as the parser expects that a catch scope always has the catch
  // variable as first and only variable.
  bool was_added;
  Variable* variable =
      Declare(zone, catch_variable_name, VariableMode::kVar, NORMAL_VARIABLE,
              kCreatedInitialized, maybe_assigned, &was_added);
  DCHECK(was_added);
  AllocateHeapSlot(variable);
}

void DeclarationScope::SetDefaults() {
  is_declaration_scope_ = true;
  has_simple_parameters_ = true;
  is_asm_module_ = false;
  force_eager_compilation_ = false;
  has_arguments_parameter_ = false;
  scope_uses_super_property_ = false;
  has_checked_syntax_ = false;
  has_this_reference_ = false;
  has_this_declaration_ =
      (is_function_scope() && !is_arrow_scope()) || is_module_scope();
  has_rest_ = false;
  receiver_ = nullptr;
  new_target_ = nullptr;
  function_ = nullptr;
  arguments_ = nullptr;
  rare_data_ = nullptr;
  should_eager_compile_ = false;
  was_lazily_parsed_ = false;
  is_skipped_function_ = false;
  preparse_data_builder_ = nullptr;
#ifdef DEBUG
  DeclarationScope* outer_declaration_scope =
      outer_scope_ ? outer_scope_->GetDeclarationScope() : nullptr;
  is_being_lazily_parsed_ =
      outer_declaration_scope ? outer_declaration_scope->is_being_lazily_parsed_
                              : false;
#endif
}

void Scope::SetDefaults() {
#ifdef DEBUG
  scope_name_ = nullptr;
  already_resolved_ = false;
  needs_migration_ = false;
#endif
  inner_scope_ = nullptr;
  sibling_ = nullptr;
  unresolved_list_.Clear();

  start_position_ = kNoSourcePosition;
  end_position_ = kNoSourcePosition;

  num_stack_slots_ = 0;
  num_heap_slots_ = Context::MIN_CONTEXT_SLOTS;

  set_language_mode(LanguageMode::kSloppy);

  scope_calls_eval_ = false;
  scope_nonlinear_ = false;
  is_hidden_ = false;
  is_debug_evaluate_scope_ = false;

  inner_scope_calls_eval_ = false;
  force_context_allocation_for_parameters_ = false;

  is_declaration_scope_ = false;

  must_use_preparsed_scope_data_ = false;
}

bool Scope::HasSimpleParameters() {
  DeclarationScope* scope = GetClosureScope();
  return !scope->is_function_scope() || scope->has_simple_parameters();
}

void DeclarationScope::set_should_eager_compile() {
  should_eager_compile_ = !was_lazily_parsed_;
}

void DeclarationScope::set_is_asm_module() { is_asm_module_ = true; }

bool Scope::IsAsmModule() const {
  return is_function_scope() && AsDeclarationScope()->is_asm_module();
}

bool Scope::ContainsAsmModule() const {
  if (IsAsmModule()) return true;

  // Check inner scopes recursively
  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    // Don't check inner functions which won't be eagerly compiled.
    if (!scope->is_function_scope() ||
        scope->AsDeclarationScope()->ShouldEagerCompile()) {
      if (scope->ContainsAsmModule()) return true;
    }
  }

  return false;
}

Scope* Scope::DeserializeScopeChain(Isolate* isolate, Zone* zone,
                                    ScopeInfo scope_info,
                                    DeclarationScope* script_scope,
                                    AstValueFactory* ast_value_factory,
                                    DeserializationMode deserialization_mode) {
  // Reconstruct the outer scope chain from a closure's context chain.
  Scope* current_scope = nullptr;
  Scope* innermost_scope = nullptr;
  Scope* outer_scope = nullptr;
  while (!scope_info.is_null()) {
    if (scope_info.scope_type() == WITH_SCOPE) {
      if (scope_info.IsDebugEvaluateScope()) {
        outer_scope = new (zone)
            DeclarationScope(zone, FUNCTION_SCOPE, handle(scope_info, isolate));
        outer_scope->set_is_debug_evaluate_scope();
      } else {
        // For scope analysis, debug-evaluate is equivalent to a with scope.
        outer_scope =
            new (zone) Scope(zone, WITH_SCOPE, handle(scope_info, isolate));
      }

    } else if (scope_info.scope_type() == SCRIPT_SCOPE) {
      // If we reach a script scope, it's the outermost scope. Install the
      // scope info of this script context onto the existing script scope to
      // avoid nesting script scopes.
      if (deserialization_mode == DeserializationMode::kIncludingVariables) {
        script_scope->SetScriptScopeInfo(handle(scope_info, isolate));
      }
      DCHECK(!scope_info.HasOuterScopeInfo());
      break;
    } else if (scope_info.scope_type() == FUNCTION_SCOPE) {
      outer_scope = new (zone)
          DeclarationScope(zone, FUNCTION_SCOPE, handle(scope_info, isolate));
      if (scope_info.IsAsmModule()) {
        outer_scope->AsDeclarationScope()->set_is_asm_module();
      }
    } else if (scope_info.scope_type() == EVAL_SCOPE) {
      outer_scope = new (zone)
          DeclarationScope(zone, EVAL_SCOPE, handle(scope_info, isolate));
    } else if (scope_info.scope_type() == CLASS_SCOPE) {
      outer_scope = new (zone)
          ClassScope(zone, ast_value_factory, handle(scope_info, isolate));
    } else if (scope_info.scope_type() == BLOCK_SCOPE) {
      if (scope_info.is_declaration_scope()) {
        outer_scope = new (zone)
            DeclarationScope(zone, BLOCK_SCOPE, handle(scope_info, isolate));
      } else {
        outer_scope =
            new (zone) Scope(zone, BLOCK_SCOPE, handle(scope_info, isolate));
      }
    } else if (scope_info.scope_type() == MODULE_SCOPE) {
      outer_scope = new (zone)
          ModuleScope(isolate, handle(scope_info, isolate), ast_value_factory);
    } else {
      DCHECK_EQ(scope_info.scope_type(), CATCH_SCOPE);
      DCHECK_EQ(scope_info.ContextLocalCount(), 1);
      DCHECK_EQ(scope_info.ContextLocalMode(0), VariableMode::kVar);
      DCHECK_EQ(scope_info.ContextLocalInitFlag(0), kCreatedInitialized);
      String name = scope_info.ContextLocalName(0);
      MaybeAssignedFlag maybe_assigned =
          scope_info.ContextLocalMaybeAssignedFlag(0);
      outer_scope = new (zone)
          Scope(zone, ast_value_factory->GetString(handle(name, isolate)),
                maybe_assigned, handle(scope_info, isolate));
    }
    if (deserialization_mode == DeserializationMode::kScopesOnly) {
      outer_scope->scope_info_ = Handle<ScopeInfo>::null();
    }
    if (current_scope != nullptr) {
      outer_scope->AddInnerScope(current_scope);
    }
    current_scope = outer_scope;
    if (innermost_scope == nullptr) innermost_scope = current_scope;
    scope_info = scope_info.HasOuterScopeInfo() ? scope_info.OuterScopeInfo()
                                                : ScopeInfo();
  }

  if (deserialization_mode == DeserializationMode::kIncludingVariables &&
      script_scope->scope_info_.is_null()) {
    Handle<ScriptContextTable> table(
        isolate->native_context()->script_context_table(), isolate);
    Handle<Context> first = ScriptContextTable::GetContext(isolate, table, 0);
    Handle<ScopeInfo> scope_info(first->scope_info(), isolate);
    script_scope->SetScriptScopeInfo(scope_info);
  }

  if (innermost_scope == nullptr) return script_scope;
  script_scope->AddInnerScope(current_scope);
  return innermost_scope;
}

DeclarationScope* Scope::AsDeclarationScope() {
  DCHECK(is_declaration_scope());
  return static_cast<DeclarationScope*>(this);
}

const DeclarationScope* Scope::AsDeclarationScope() const {
  DCHECK(is_declaration_scope());
  return static_cast<const DeclarationScope*>(this);
}

ModuleScope* Scope::AsModuleScope() {
  DCHECK(is_module_scope());
  return static_cast<ModuleScope*>(this);
}

const ModuleScope* Scope::AsModuleScope() const {
  DCHECK(is_module_scope());
  return static_cast<const ModuleScope*>(this);
}

ClassScope* Scope::AsClassScope() {
  DCHECK(is_class_scope());
  return static_cast<ClassScope*>(this);
}

const ClassScope* Scope::AsClassScope() const {
  DCHECK(is_class_scope());
  return static_cast<const ClassScope*>(this);
}

void DeclarationScope::DeclareSloppyBlockFunction(
    SloppyBlockFunctionStatement* sloppy_block_function) {
  sloppy_block_functions_.Add(sloppy_block_function);
}

void DeclarationScope::HoistSloppyBlockFunctions(AstNodeFactory* factory) {
  DCHECK(is_sloppy(language_mode()));
  DCHECK(is_function_scope() || is_eval_scope() || is_script_scope() ||
         (is_block_scope() && outer_scope()->is_function_scope()));
  DCHECK(HasSimpleParameters() || is_block_scope() || is_being_lazily_parsed_);
  DCHECK_EQ(factory == nullptr, is_being_lazily_parsed_);

  if (sloppy_block_functions_.is_empty()) return;

  // In case of complex parameters the current scope is the body scope and the
  // parameters are stored in the outer scope.
  Scope* parameter_scope = HasSimpleParameters() ? this : outer_scope_;
  DCHECK(parameter_scope->is_function_scope() || is_eval_scope() ||
         is_script_scope());

  DeclarationScope* decl_scope = this;
  while (decl_scope->is_eval_scope()) {
    decl_scope = decl_scope->outer_scope()->GetDeclarationScope();
  }
  Scope* outer_scope = decl_scope->outer_scope();

  // For each variable which is used as a function declaration in a sloppy
  // block,
  for (SloppyBlockFunctionStatement* sloppy_block_function :
       sloppy_block_functions_) {
    const AstRawString* name = sloppy_block_function->name();

    // If the variable wouldn't conflict with a lexical declaration
    // or parameter,

    // Check if there's a conflict with a parameter.
    Variable* maybe_parameter = parameter_scope->LookupLocal(name);
    if (maybe_parameter != nullptr && maybe_parameter->is_parameter()) {
      continue;
    }

    // Check if there's a conflict with a lexical declaration
    Scope* query_scope = sloppy_block_function->scope()->outer_scope();
    Variable* var = nullptr;
    bool should_hoist = true;

    // It is not sufficient to just do a Lookup on query_scope: for
    // example, that does not prevent hoisting of the function in
    // `{ let e; try {} catch (e) { function e(){} } }`
    do {
      var = query_scope->LookupInScopeOrScopeInfo(name);
      if (var != nullptr && IsLexicalVariableMode(var->mode())) {
        should_hoist = false;
        break;
      }
      query_scope = query_scope->outer_scope();
    } while (query_scope != outer_scope);

    if (!should_hoist) continue;

    if (factory) {
      DCHECK(!is_being_lazily_parsed_);
      int pos = sloppy_block_function->position();
      bool ok = true;
      bool was_added;
      auto declaration = factory->NewVariableDeclaration(pos);
      // Based on the preceding checks, it doesn't matter what we pass as
      // sloppy_mode_block_scope_function_redefinition.
      Variable* var = DeclareVariable(
          declaration, name, pos, VariableMode::kVar, NORMAL_VARIABLE,
          Variable::DefaultInitializationFlag(VariableMode::kVar), &was_added,
          nullptr, &ok);
      DCHECK(ok);
      VariableProxy* source =
          factory->NewVariableProxy(sloppy_block_function->var());
      VariableProxy* target = factory->NewVariableProxy(var);
      Assignment* assignment = factory->NewAssignment(
          sloppy_block_function->init(), target, source, pos);
      assignment->set_lookup_hoisting_mode(LookupHoistingMode::kLegacySloppy);
      Statement* statement = factory->NewExpressionStatement(assignment, pos);
      sloppy_block_function->set_statement(statement);
    } else {
      DCHECK(is_being_lazily_parsed_);
      bool was_added;
      Variable* var = DeclareVariableName(name, VariableMode::kVar, &was_added);
      if (sloppy_block_function->init() == Token::ASSIGN) {
        var->SetMaybeAssigned();
      }
    }
  }
}

bool DeclarationScope::Analyze(ParseInfo* info) {
  RuntimeCallTimerScope runtimeTimer(
      info->runtime_call_stats(),
      info->on_background_thread()
          ? RuntimeCallCounterId::kCompileBackgroundScopeAnalysis
          : RuntimeCallCounterId::kCompileScopeAnalysis);
  DCHECK_NOT_NULL(info->literal());
  DeclarationScope* scope = info->literal()->scope();

  base::Optional<AllowHandleDereference> allow_deref;
  if (!info->maybe_outer_scope_info().is_null()) {
    // Allow dereferences to the scope info if there is one.
    allow_deref.emplace();
  }

  if (scope->is_eval_scope() && is_sloppy(scope->language_mode())) {
    AstNodeFactory factory(info->ast_value_factory(), info->zone());
    scope->HoistSloppyBlockFunctions(&factory);
  }

  // We are compiling one of four cases:
  // 1) top-level code,
  // 2) a function/eval/module on the top-level
  // 3) a function/eval in a scope that was already resolved.
  DCHECK(scope->is_script_scope() || scope->outer_scope()->is_script_scope() ||
         scope->outer_scope()->already_resolved_);

  // The outer scope is never lazy.
  scope->set_should_eager_compile();

  if (scope->must_use_preparsed_scope_data_) {
    DCHECK_EQ(scope->scope_type_, ScopeType::FUNCTION_SCOPE);
    allow_deref.emplace();
    info->consumed_preparse_data()->RestoreScopeAllocationData(scope);
  }

  if (!scope->AllocateVariables(info)) return false;

#ifdef DEBUG
  if (FLAG_print_scopes) {
    PrintF("Global scope:\n");
    scope->Print();
  }
  scope->CheckScopePositions();
  scope->CheckZones();
#endif

  return true;
}

void DeclarationScope::DeclareThis(AstValueFactory* ast_value_factory) {
  DCHECK(has_this_declaration());

  bool derived_constructor = IsDerivedConstructor(function_kind_);

  receiver_ = new (zone())
      Variable(this, ast_value_factory->this_string(),
               derived_constructor ? VariableMode::kConst : VariableMode::kVar,
               THIS_VARIABLE,
               derived_constructor ? kNeedsInitialization : kCreatedInitialized,
               kNotAssigned);
}

void DeclarationScope::DeclareArguments(AstValueFactory* ast_value_factory) {
  DCHECK(is_function_scope());
  DCHECK(!is_arrow_scope());

  // Declare 'arguments' variable which exists in all non arrow functions.  Note
  // that it might never be accessed, in which case it won't be allocated during
  // variable allocation.
  bool was_added;
  arguments_ =
      Declare(zone(), ast_value_factory->arguments_string(), VariableMode::kVar,
              NORMAL_VARIABLE, kCreatedInitialized, kNotAssigned, &was_added);
  if (!was_added && IsLexicalVariableMode(arguments_->mode())) {
    // Check if there's lexically declared variable named arguments to avoid
    // redeclaration. See ES#sec-functiondeclarationinstantiation, step 20.
    arguments_ = nullptr;
  }
}

void DeclarationScope::DeclareDefaultFunctionVariables(
    AstValueFactory* ast_value_factory) {
  DCHECK(is_function_scope());
  DCHECK(!is_arrow_scope());

  DeclareThis(ast_value_factory);
  bool was_added;
  new_target_ = Declare(zone(), ast_value_factory->new_target_string(),
                        VariableMode::kConst, NORMAL_VARIABLE,
                        kCreatedInitialized, kNotAssigned, &was_added);
  DCHECK(was_added);

  if (IsConciseMethod(function_kind_) || IsClassConstructor(function_kind_) ||
      IsAccessorFunction(function_kind_)) {
    EnsureRareData()->this_function = Declare(
        zone(), ast_value_factory->this_function_string(), VariableMode::kConst,
        NORMAL_VARIABLE, kCreatedInitialized, kNotAssigned, &was_added);
    DCHECK(was_added);
  }
}

Variable* DeclarationScope::DeclareFunctionVar(const AstRawString* name,
                                               Scope* cache) {
  DCHECK(is_function_scope());
  DCHECK_NULL(function_);
  if (cache == nullptr) cache = this;
  DCHECK_NULL(cache->variables_.Lookup(name));
  VariableKind kind = is_sloppy(language_mode()) ? SLOPPY_FUNCTION_NAME_VARIABLE
                                                 : NORMAL_VARIABLE;
  function_ = new (zone())
      Variable(this, name, VariableMode::kConst, kind, kCreatedInitialized);
  if (calls_sloppy_eval()) {
    cache->NonLocal(name, VariableMode::kDynamic);
  } else {
    cache->variables_.Add(zone(), function_);
  }
  return function_;
}

Variable* DeclarationScope::DeclareGeneratorObjectVar(
    const AstRawString* name) {
  DCHECK(is_function_scope() || is_module_scope());
  DCHECK_NULL(generator_object_var());

  Variable* result = EnsureRareData()->generator_object =
      NewTemporary(name, kNotAssigned);
  result->set_is_used();
  return result;
}

Scope* Scope::FinalizeBlockScope() {
  DCHECK(is_block_scope());
#ifdef DEBUG
  DCHECK_NE(sibling_, this);
#endif

  if (variables_.occupancy() > 0 ||
      (is_declaration_scope() && AsDeclarationScope()->calls_sloppy_eval())) {
    return this;
  }

  DCHECK(!is_class_scope());

  // Remove this scope from outer scope.
  outer_scope()->RemoveInnerScope(this);

  // Reparent inner scopes.
  if (inner_scope_ != nullptr) {
    Scope* scope = inner_scope_;
    scope->outer_scope_ = outer_scope();
    while (scope->sibling_ != nullptr) {
      scope = scope->sibling_;
      scope->outer_scope_ = outer_scope();
    }
    scope->sibling_ = outer_scope()->inner_scope_;
    outer_scope()->inner_scope_ = inner_scope_;
    inner_scope_ = nullptr;
  }

  // Move unresolved variables
  if (!unresolved_list_.is_empty()) {
    outer_scope()->unresolved_list_.Prepend(std::move(unresolved_list_));
    unresolved_list_.Clear();
  }

  if (inner_scope_calls_eval_) outer_scope()->inner_scope_calls_eval_ = true;

  // No need to propagate scope_calls_eval_, since if it was relevant to
  // this scope we would have had to bail out at the top.
  DCHECK(!scope_calls_eval_ || !is_declaration_scope() ||
         !is_sloppy(language_mode()));

  // This block does not need a context.
  num_heap_slots_ = 0;

  // Mark scope as removed by making it its own sibling.
#ifdef DEBUG
  sibling_ = this;
#endif

  return nullptr;
}

void DeclarationScope::AddLocal(Variable* var) {
  DCHECK(!already_resolved_);
  // Temporaries are only placed in ClosureScopes.
  DCHECK_EQ(GetClosureScope(), this);
  locals_.Add(var);
}

void Scope::Snapshot::Reparent(DeclarationScope* new_parent) {
  DCHECK(!IsCleared());
  DCHECK_EQ(new_parent, outer_scope_and_calls_eval_.GetPointer()->inner_scope_);
  DCHECK_EQ(new_parent->outer_scope_, outer_scope_and_calls_eval_.GetPointer());
  DCHECK_EQ(new_parent, new_parent->GetClosureScope());
  DCHECK_NULL(new_parent->inner_scope_);
  DCHECK(new_parent->unresolved_list_.is_empty());
  Scope* inner_scope = new_parent->sibling_;
  if (inner_scope != top_inner_scope_) {
    for (; inner_scope->sibling() != top_inner_scope_;
         inner_scope = inner_scope->sibling()) {
      inner_scope->outer_scope_ = new_parent;
      if (inner_scope->inner_scope_calls_eval_) {
        new_parent->inner_scope_calls_eval_ = true;
      }
      DCHECK_NE(inner_scope, new_parent);
    }
    inner_scope->outer_scope_ = new_parent;
    if (inner_scope->inner_scope_calls_eval_) {
      new_parent->inner_scope_calls_eval_ = true;
    }
    new_parent->inner_scope_ = new_parent->sibling_;
    inner_scope->sibling_ = nullptr;
    // Reset the sibling rather than the inner_scope_ since we
    // want to keep new_parent there.
    new_parent->sibling_ = top_inner_scope_;
  }

  Scope* outer_scope_ = outer_scope_and_calls_eval_.GetPointer();
  new_parent->unresolved_list_.MoveTail(&outer_scope_->unresolved_list_,
                                        top_unresolved_);

  // Move temporaries allocated for complex parameter initializers.
  DeclarationScope* outer_closure = outer_scope_->GetClosureScope();
  for (auto it = top_local_; it != outer_closure->locals()->end(); ++it) {
    Variable* local = *it;
    DCHECK_EQ(VariableMode::kTemporary, local->mode());
    DCHECK_EQ(local->scope(), local->scope()->GetClosureScope());
    DCHECK_NE(local->scope(), new_parent);
    local->set_scope(new_parent);
  }
  new_parent->locals_.MoveTail(outer_closure->locals(), top_local_);
  outer_closure->locals_.Rewind(top_local_);

  // Move eval calls since Snapshot's creation into new_parent.
  if (outer_scope_and_calls_eval_->scope_calls_eval_) {
    new_parent->scope_calls_eval_ = true;
    new_parent->inner_scope_calls_eval_ = true;
  }

  // We are in the arrow function case. The calls eval we may have recorded
  // is intended for the inner scope and we should simply restore the
  // original "calls eval" flag of the outer scope.
  RestoreEvalFlag();
  Clear();
}

void Scope::ReplaceOuterScope(Scope* outer) {
  DCHECK_NOT_NULL(outer);
  DCHECK_NOT_NULL(outer_scope_);
  DCHECK(!already_resolved_);
  outer_scope_->RemoveInnerScope(this);
  outer->AddInnerScope(this);
  outer_scope_ = outer;
}

Variable* Scope::LookupInScopeInfo(const AstRawString* name, Scope* cache) {
  DCHECK(!scope_info_.is_null());
  DCHECK_NULL(cache->variables_.Lookup(name));
  DisallowHeapAllocation no_gc;

  String name_handle = *name->string();
  // The Scope is backed up by ScopeInfo. This means it cannot operate in a
  // heap-independent mode, and all strings must be internalized immediately. So
  // it's ok to get the Handle<String> here.
  bool found = false;

  VariableLocation location;
  int index;
  VariableMode mode;
  InitializationFlag init_flag;
  MaybeAssignedFlag maybe_assigned_flag;

  {
    location = VariableLocation::CONTEXT;
    index = ScopeInfo::ContextSlotIndex(*scope_info_, name_handle, &mode,
                                        &init_flag, &maybe_assigned_flag);
    found = index >= 0;
  }

  if (!found && is_module_scope()) {
    location = VariableLocation::MODULE;
    index = scope_info_->ModuleIndex(name_handle, &mode, &init_flag,
                                     &maybe_assigned_flag);
    found = index != 0;
  }

  if (!found) {
    index = scope_info_->FunctionContextSlotIndex(name_handle);
    if (index < 0) return nullptr;  // Nowhere found.
    Variable* var = AsDeclarationScope()->DeclareFunctionVar(name, cache);
    DCHECK_EQ(VariableMode::kConst, var->mode());
    var->AllocateTo(VariableLocation::CONTEXT, index);
    return cache->variables_.Lookup(name);
  }

  if (!is_module_scope()) {
    DCHECK_NE(index, scope_info_->ReceiverContextSlotIndex());
  }

  bool was_added;
  Variable* var =
      cache->variables_.Declare(zone(), this, name, mode, NORMAL_VARIABLE,
                                init_flag, maybe_assigned_flag, &was_added);
  DCHECK(was_added);
  var->AllocateTo(location, index);
  return var;
}

Variable* DeclarationScope::DeclareParameter(const AstRawString* name,
                                             VariableMode mode,
                                             bool is_optional, bool is_rest,
                                             AstValueFactory* ast_value_factory,
                                             int position) {
  DCHECK(!already_resolved_);
  DCHECK(is_function_scope() || is_module_scope());
  DCHECK(!has_rest_);
  DCHECK(!is_optional || !is_rest);
  DCHECK(!is_being_lazily_parsed_);
  DCHECK(!was_lazily_parsed_);
  Variable* var;
  if (mode == VariableMode::kTemporary) {
    var = NewTemporary(name);
  } else {
    var = LookupLocal(name);
    DCHECK_EQ(mode, VariableMode::kVar);
    DCHECK(var->is_parameter());
  }
  has_rest_ = is_rest;
  var->set_initializer_position(position);
  params_.Add(var, zone());
  if (!is_rest) ++num_parameters_;
  if (name == ast_value_factory->arguments_string()) {
    has_arguments_parameter_ = true;
  }
  // Params are automatically marked as used to make sure that the debugger and
  // function.arguments sees them.
  // TODO(verwaest): Reevaluate whether we always need to do this, since
  // strict-mode function.arguments does not make the arguments available.
  var->set_is_used();
  return var;
}

void DeclarationScope::RecordParameter(bool is_rest) {
  DCHECK(!already_resolved_);
  DCHECK(is_function_scope() || is_module_scope());
  DCHECK(is_being_lazily_parsed_);
  DCHECK(!has_rest_);
  has_rest_ = is_rest;
  if (!is_rest) ++num_parameters_;
}

Variable* Scope::DeclareLocal(const AstRawString* name, VariableMode mode,
                              VariableKind kind, bool* was_added,
                              InitializationFlag init_flag) {
  DCHECK(!already_resolved_);
  // Private methods should be declared with ClassScope::DeclarePrivateName()
  DCHECK(!IsPrivateMethodOrAccessorVariableMode(mode));
  // This function handles VariableMode::kVar, VariableMode::kLet, and
  // VariableMode::kConst modes.  VariableMode::kDynamic variables are
  // introduced during variable allocation, and VariableMode::kTemporary
  // variables are allocated via NewTemporary().
  DCHECK(IsDeclaredVariableMode(mode));
  DCHECK_IMPLIES(GetDeclarationScope()->is_being_lazily_parsed(),
                 mode == VariableMode::kVar || mode == VariableMode::kLet ||
                     mode == VariableMode::kConst);
  DCHECK(!GetDeclarationScope()->was_lazily_parsed());
  Variable* var =
      Declare(zone(), name, mode, kind, init_flag, kNotAssigned, was_added);

  // Pessimistically assume that top-level variables will be assigned and used.
  //
  // Top-level variables in a script can be accessed by other scripts or even
  // become global properties. While this does not apply to top-level variables
  // in a module (assuming they are not exported), we must still mark these as
  // assigned because they might be accessed by a lazily parsed top-level
  // function, which, for efficiency, we preparse without variable tracking.
  if (is_script_scope() || is_module_scope()) {
    if (mode != VariableMode::kConst) var->SetMaybeAssigned();
    var->set_is_used();
  }

  return var;
}

Variable* Scope::DeclareVariable(
    Declaration* declaration, const AstRawString* name, int pos,
    VariableMode mode, VariableKind kind, InitializationFlag init,
    bool* was_added, bool* sloppy_mode_block_scope_function_redefinition,
    bool* ok) {
  // Private methods should be declared with ClassScope::DeclarePrivateName()
  DCHECK(!IsPrivateMethodOrAccessorVariableMode(mode));
  DCHECK(IsDeclaredVariableMode(mode));
  DCHECK(!already_resolved_);
  DCHECK(!GetDeclarationScope()->is_being_lazily_parsed());
  DCHECK(!GetDeclarationScope()->was_lazily_parsed());

  if (mode == VariableMode::kVar && !is_declaration_scope()) {
    return GetDeclarationScope()->DeclareVariable(
        declaration, name, pos, mode, kind, init, was_added,
        sloppy_mode_block_scope_function_redefinition, ok);
  }
  DCHECK(!is_catch_scope());
  DCHECK(!is_with_scope());
  DCHECK(is_declaration_scope() ||
         (IsLexicalVariableMode(mode) && is_block_scope()));

  DCHECK_NOT_NULL(name);

  Variable* var = LookupLocal(name);
  // Declare the variable in the declaration scope.
  *was_added = var == nullptr;
  if (V8_LIKELY(*was_added)) {
    if (V8_UNLIKELY(is_eval_scope() && is_sloppy(language_mode()) &&
                    mode == VariableMode::kVar)) {
      // In a var binding in a sloppy direct eval, pollute the enclosing scope
      // with this new binding by doing the following:
      // The proxy is bound to a lookup variable to force a dynamic declaration
      // using the DeclareEvalVar or DeclareEvalFunction runtime functions.
      DCHECK_EQ(NORMAL_VARIABLE, kind);
      var = NonLocal(name, VariableMode::kDynamic);
      // Mark the var as used in case anyone outside the eval wants to use it.
      var->set_is_used();
    } else {
      // Declare the name.
      var = DeclareLocal(name, mode, kind, was_added, init);
      DCHECK(*was_added);
    }
  } else {
    var->SetMaybeAssigned();
    if (V8_UNLIKELY(IsLexicalVariableMode(mode) ||
                    IsLexicalVariableMode(var->mode()))) {
      // The name was declared in this scope before; check for conflicting
      // re-declarations. We have a conflict if either of the declarations is
      // not a var (in script scope, we also have to ignore legacy const for
      // compatibility). There is similar code in runtime.cc in the Declare
      // functions. The function CheckConflictingVarDeclarations checks for
      // var and let bindings from different scopes whereas this is a check
      // for conflicting declarations within the same scope. This check also
      // covers the special case
      //
      // function () { let x; { var x; } }
      //
      // because the var declaration is hoisted to the function scope where
      // 'x' is already bound.
      //
      // In harmony we treat re-declarations as early errors. See ES5 16 for a
      // definition of early errors.
      //
      // Allow duplicate function decls for web compat, see bug 4693.
      *ok = var->is_sloppy_block_function() &&
            kind == SLOPPY_BLOCK_FUNCTION_VARIABLE;
      *sloppy_mode_block_scope_function_redefinition = *ok;
    }
  }
  DCHECK_NOT_NULL(var);

  // We add a declaration node for every declaration. The compiler
  // will only generate code if necessary. In particular, declarations
  // for inner local variables that do not represent functions won't
  // result in any generated code.
  //
  // This will lead to multiple declaration nodes for the
  // same variable if it is declared several times. This is not a
  // semantic issue, but it may be a performance issue since it may
  // lead to repeated DeclareEvalVar or DeclareEvalFunction calls.
  decls_.Add(declaration);
  declaration->set_var(var);
  return var;
}

Variable* Scope::DeclareVariableName(const AstRawString* name,
                                     VariableMode mode, bool* was_added,
                                     VariableKind kind) {
  DCHECK(IsDeclaredVariableMode(mode));
  DCHECK(!already_resolved_);
  DCHECK(GetDeclarationScope()->is_being_lazily_parsed());
  // Private methods should be declared with ClassScope::DeclarePrivateName()
  DCHECK(!IsPrivateMethodOrAccessorVariableMode(mode));
  if (mode == VariableMode::kVar && !is_declaration_scope()) {
    return GetDeclarationScope()->DeclareVariableName(name, mode, was_added,
                                                      kind);
  }
  DCHECK(!is_with_scope());
  DCHECK(!is_eval_scope());
  DCHECK(is_declaration_scope() || IsLexicalVariableMode(mode));
  DCHECK(scope_info_.is_null());

  // Declare the variable in the declaration scope.
  Variable* var = DeclareLocal(name, mode, kind, was_added);
  if (!*was_added) {
    if (IsLexicalVariableMode(mode) || IsLexicalVariableMode(var->mode())) {
      if (!var->is_sloppy_block_function() ||
          kind != SLOPPY_BLOCK_FUNCTION_VARIABLE) {
        // Duplicate functions are allowed in the sloppy mode, but if this is
        // not a function declaration, it's an error. This is an error PreParser
        // hasn't previously detected.
        return nullptr;
      }
      // Sloppy block function redefinition.
    }
    var->SetMaybeAssigned();
  }
  var->set_is_used();
  return var;
}

Variable* Scope::DeclareCatchVariableName(const AstRawString* name) {
  DCHECK(!already_resolved_);
  DCHECK(is_catch_scope());
  DCHECK(scope_info_.is_null());

  bool was_added;
  Variable* result = Declare(zone(), name, VariableMode::kVar, NORMAL_VARIABLE,
                             kCreatedInitialized, kNotAssigned, &was_added);
  DCHECK(was_added);
  return result;
}

void Scope::AddUnresolved(VariableProxy* proxy) {
  DCHECK(!already_resolved_);
  DCHECK(!proxy->is_resolved());
  unresolved_list_.Add(proxy);
}

Variable* DeclarationScope::DeclareDynamicGlobal(const AstRawString* name,
                                                 VariableKind kind,
                                                 Scope* cache) {
  DCHECK(is_script_scope());
  bool was_added;
  return cache->variables_.Declare(
      zone(), this, name, VariableMode::kDynamicGlobal, kind,
      kCreatedInitialized, kNotAssigned, &was_added);
  // TODO(neis): Mark variable as maybe-assigned?
}

bool Scope::RemoveUnresolved(VariableProxy* var) {
  return unresolved_list_.Remove(var);
}

void Scope::DeleteUnresolved(VariableProxy* var) {
  DCHECK(unresolved_list_.Contains(var));
  var->mark_removed_from_unresolved();
}

Variable* Scope::NewTemporary(const AstRawString* name) {
  return NewTemporary(name, kMaybeAssigned);
}

Variable* Scope::NewTemporary(const AstRawString* name,
                              MaybeAssignedFlag maybe_assigned) {
  DeclarationScope* scope = GetClosureScope();
  Variable* var = new (zone()) Variable(scope, name, VariableMode::kTemporary,
                                        NORMAL_VARIABLE, kCreatedInitialized);
  scope->AddLocal(var);
  if (maybe_assigned == kMaybeAssigned) var->SetMaybeAssigned();
  return var;
}

Declaration* DeclarationScope::CheckConflictingVarDeclarations() {
  if (has_checked_syntax_) return nullptr;
  for (Declaration* decl : decls_) {
    // Lexical vs lexical conflicts within the same scope have already been
    // captured in Parser::Declare. The only conflicts we still need to check
    // are lexical vs nested var.
    if (decl->IsVariableDeclaration() &&
        decl->AsVariableDeclaration()->AsNested() != nullptr) {
      Scope* current = decl->AsVariableDeclaration()->AsNested()->scope();
      DCHECK(decl->var()->mode() == VariableMode::kVar ||
             decl->var()->mode() == VariableMode::kDynamic);
      // Iterate through all scopes until the declaration scope.
      do {
        // There is a conflict if there exists a non-VAR binding.
        if (current->is_catch_scope()) {
          current = current->outer_scope();
          continue;
        }
        Variable* other_var = current->LookupLocal(decl->var()->raw_name());
        if (other_var != nullptr) {
          DCHECK(IsLexicalVariableMode(other_var->mode()));
          return decl;
        }
        current = current->outer_scope();
      } while (current != this);
    }
  }

  if (V8_LIKELY(!is_eval_scope())) return nullptr;
  if (!is_sloppy(language_mode())) return nullptr;

  // Var declarations in sloppy eval are hoisted to the first non-eval
  // declaration scope. Check for conflicts between the eval scope that
  // declaration scope.
  Scope* end = this;
  do {
    end = end->outer_scope_->GetDeclarationScope();
  } while (end->is_eval_scope());
  end = end->outer_scope_;

  for (Declaration* decl : decls_) {
    if (IsLexicalVariableMode(decl->var()->mode())) continue;
    Scope* current = outer_scope_;
    // Iterate through all scopes until and including the declaration scope.
    do {
      // There is a conflict if there exists a non-VAR binding up to the
      // declaration scope in which this sloppy-eval runs.
      Variable* other_var =
          current->LookupInScopeOrScopeInfo(decl->var()->raw_name());
      if (other_var != nullptr && IsLexicalVariableMode(other_var->mode())) {
        DCHECK(!current->is_catch_scope());
        return decl;
      }
      current = current->outer_scope();
    } while (current != end);
  }
  return nullptr;
}

const AstRawString* Scope::FindVariableDeclaredIn(Scope* scope,
                                                  VariableMode mode_limit) {
  const VariableMap& variables = scope->variables_;
  for (ZoneHashMap::Entry* p = variables.Start(); p != nullptr;
       p = variables.Next(p)) {
    const AstRawString* name = static_cast<const AstRawString*>(p->key);
    Variable* var = LookupLocal(name);
    if (var != nullptr && var->mode() <= mode_limit) return name;
  }
  return nullptr;
}

void DeclarationScope::DeserializeReceiver(AstValueFactory* ast_value_factory) {
  if (is_script_scope()) {
    DCHECK_NOT_NULL(receiver_);
    return;
  }
  DCHECK(has_this_declaration());
  DeclareThis(ast_value_factory);
  if (is_debug_evaluate_scope()) {
    receiver_->AllocateTo(VariableLocation::LOOKUP, -1);
  } else {
    receiver_->AllocateTo(VariableLocation::CONTEXT,
                          scope_info_->ReceiverContextSlotIndex());
  }
}

bool DeclarationScope::AllocateVariables(ParseInfo* info) {
  // Module variables must be allocated before variable resolution
  // to ensure that UpdateNeedsHoleCheck() can detect import variables.
  if (is_module_scope()) AsModuleScope()->AllocateModuleVariables();

  ClassScope* closest_class_scope = GetClassScope();
  if (closest_class_scope != nullptr &&
      !closest_class_scope->ResolvePrivateNames(info)) {
    DCHECK(info->pending_error_handler()->has_pending_error());
    return false;
  }

  if (!ResolveVariablesRecursively(info)) {
    DCHECK(info->pending_error_handler()->has_pending_error());
    return false;
  }

  // // Don't allocate variables of preparsed scopes.
  if (!was_lazily_parsed()) AllocateVariablesRecursively();

  return true;
}

bool Scope::HasThisReference() const {
  if (is_declaration_scope() && AsDeclarationScope()->has_this_reference()) {
    return true;
  }

  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    if (!scope->is_declaration_scope() ||
        !scope->AsDeclarationScope()->has_this_declaration()) {
      if (scope->HasThisReference()) return true;
    }
  }

  return false;
}

bool Scope::AllowsLazyParsingWithoutUnresolvedVariables(
    const Scope* outer) const {
  // If none of the outer scopes need to decide whether to context allocate
  // specific variables, we can preparse inner functions without unresolved
  // variables. Otherwise we need to find unresolved variables to force context
  // allocation of the matching declarations. We can stop at the outer scope for
  // the parse, since context allocation of those variables is already
  // guaranteed to be correct.
  for (const Scope* s = this; s != outer; s = s->outer_scope_) {
    // Eval forces context allocation on all outer scopes, so we don't need to
    // look at those scopes. Sloppy eval makes top-level non-lexical variables
    // dynamic, whereas strict-mode requires context allocation.
    if (s->is_eval_scope()) return is_sloppy(s->language_mode());
    // Catch scopes force context allocation of all variables.
    if (s->is_catch_scope()) continue;
    // With scopes do not introduce variables that need allocation.
    if (s->is_with_scope()) continue;
    DCHECK(s->is_module_scope() || s->is_block_scope() ||
           s->is_function_scope());
    return false;
  }
  return true;
}

bool DeclarationScope::AllowsLazyCompilation() const {
  // Functions which force eager compilation and class member initializer
  // functions are not lazily compilable.
  return !force_eager_compilation_ &&
         !IsClassMembersInitializerFunction(function_kind());
}

int Scope::ContextChainLength(Scope* scope) const {
  int n = 0;
  for (const Scope* s = this; s != scope; s = s->outer_scope_) {
    DCHECK_NOT_NULL(s);  // scope must be in the scope chain
    if (s->NeedsContext()) n++;
  }
  return n;
}

int Scope::ContextChainLengthUntilOutermostSloppyEval() const {
  int result = 0;
  int length = 0;

  for (const Scope* s = this; s != nullptr; s = s->outer_scope()) {
    if (!s->NeedsContext()) continue;
    length++;
    if (s->is_declaration_scope() &&
        s->AsDeclarationScope()->calls_sloppy_eval()) {
      result = length;
    }
  }

  return result;
}

ClassScope* Scope::GetClassScope() {
  Scope* scope = this;
  while (scope != nullptr && !scope->is_class_scope()) {
    scope = scope->outer_scope();
  }
  if (scope != nullptr && scope->is_class_scope()) {
    return scope->AsClassScope();
  }
  return nullptr;
}

DeclarationScope* Scope::GetDeclarationScope() {
  Scope* scope = this;
  while (!scope->is_declaration_scope()) {
    scope = scope->outer_scope();
  }
  return scope->AsDeclarationScope();
}

const DeclarationScope* Scope::GetClosureScope() const {
  const Scope* scope = this;
  while (!scope->is_declaration_scope() || scope->is_block_scope()) {
    scope = scope->outer_scope();
  }
  return scope->AsDeclarationScope();
}

DeclarationScope* Scope::GetClosureScope() {
  Scope* scope = this;
  while (!scope->is_declaration_scope() || scope->is_block_scope()) {
    scope = scope->outer_scope();
  }
  return scope->AsDeclarationScope();
}

bool Scope::NeedsScopeInfo() const {
  DCHECK(!already_resolved_);
  DCHECK(GetClosureScope()->ShouldEagerCompile());
  // The debugger expects all functions to have scope infos.
  // TODO(jochen|yangguo): Remove this requirement.
  if (is_function_scope()) return true;
  return NeedsContext();
}

bool Scope::ShouldBanArguments() {
  return GetReceiverScope()->should_ban_arguments();
}

DeclarationScope* Scope::GetReceiverScope() {
  Scope* scope = this;
  while (!scope->is_declaration_scope() ||
         (!scope->is_script_scope() &&
          !scope->AsDeclarationScope()->has_this_declaration())) {
    scope = scope->outer_scope();
  }
  return scope->AsDeclarationScope();
}

Scope* Scope::GetOuterScopeWithContext() {
  Scope* scope = outer_scope_;
  while (scope && !scope->NeedsContext()) {
    scope = scope->outer_scope();
  }
  return scope;
}

namespace {
bool WasLazilyParsed(Scope* scope) {
  return scope->is_declaration_scope() &&
         scope->AsDeclarationScope()->was_lazily_parsed();
}

}  // namespace

template <typename FunctionType>
void Scope::ForEach(FunctionType callback) {
  Scope* scope = this;
  while (true) {
    Iteration iteration = callback(scope);
    // Try to descend into inner scopes first.
    if ((iteration == Iteration::kDescend) && scope->inner_scope_ != nullptr) {
      scope = scope->inner_scope_;
    } else {
      // Find the next outer scope with a sibling.
      while (scope->sibling_ == nullptr) {
        if (scope == this) return;
        scope = scope->outer_scope_;
      }
      if (scope == this) return;
      scope = scope->sibling_;
    }
  }
}

void Scope::CollectNonLocals(DeclarationScope* max_outer_scope,
                             Isolate* isolate, ParseInfo* info,
                             Handle<StringSet>* non_locals) {
  this->ForEach([max_outer_scope, isolate, info, non_locals](Scope* scope) {
    // Module variables must be allocated before variable resolution
    // to ensure that UpdateNeedsHoleCheck() can detect import variables.
    if (scope->is_module_scope()) {
      scope->AsModuleScope()->AllocateModuleVariables();
    }

    // Lazy parsed declaration scopes are already partially analyzed. If there
    // are unresolved references remaining, they just need to be resolved in
    // outer scopes.
    Scope* lookup = WasLazilyParsed(scope) ? scope->outer_scope() : scope;

    for (VariableProxy* proxy : scope->unresolved_list_) {
      DCHECK(!proxy->is_resolved());
      Variable* var =
          Lookup<kParsedScope>(proxy, lookup, max_outer_scope->outer_scope());
      if (var == nullptr) {
        *non_locals = StringSet::Add(isolate, *non_locals, proxy->name());
      } else {
        // In this case we need to leave scopes in a way that they can be
        // allocated. If we resolved variables from lazy parsed scopes, we need
        // to context allocate the var.
        scope->ResolveTo(info, proxy, var);
        if (!var->is_dynamic() && lookup != scope)
          var->ForceContextAllocation();
      }
    }

    // Clear unresolved_list_ as it's in an inconsistent state.
    scope->unresolved_list_.Clear();
    return Iteration::kDescend;
  });
}

void Scope::AnalyzePartially(DeclarationScope* max_outer_scope,
                             AstNodeFactory* ast_node_factory,
                             UnresolvedList* new_unresolved_list) {
  this->ForEach([max_outer_scope, ast_node_factory,
                 new_unresolved_list](Scope* scope) {
    DCHECK_IMPLIES(scope->is_declaration_scope(),
                   !scope->AsDeclarationScope()->was_lazily_parsed());

    for (VariableProxy* proxy = scope->unresolved_list_.first();
         proxy != nullptr; proxy = proxy->next_unresolved()) {
      DCHECK(!proxy->is_resolved());
      Variable* var =
          Lookup<kParsedScope>(proxy, scope, max_outer_scope->outer_scope());
      if (var == nullptr) {
        // Don't copy unresolved references to the script scope, unless it's a
        // reference to a private name or method. In that case keep it so we
        // can fail later.
        if (!max_outer_scope->outer_scope()->is_script_scope()) {
          VariableProxy* copy = ast_node_factory->CopyVariableProxy(proxy);
          new_unresolved_list->Add(copy);
        }
      } else {
        var->set_is_used();
        if (proxy->is_assigned()) var->SetMaybeAssigned();
      }
    }

    // Clear unresolved_list_ as it's in an inconsistent state.
    scope->unresolved_list_.Clear();
    return Iteration::kDescend;
  });
}

Handle<StringSet> DeclarationScope::CollectNonLocals(
    Isolate* isolate, ParseInfo* info, Handle<StringSet> non_locals) {
  Scope::CollectNonLocals(this, isolate, info, &non_locals);
  return non_locals;
}

void DeclarationScope::ResetAfterPreparsing(AstValueFactory* ast_value_factory,
                                            bool aborted) {
  DCHECK(is_function_scope());

  // Reset all non-trivial members.
  params_.Clear();
  decls_.Clear();
  locals_.Clear();
  inner_scope_ = nullptr;
  unresolved_list_.Clear();
  sloppy_block_functions_.Clear();
  rare_data_ = nullptr;
  has_rest_ = false;

  DCHECK_NE(zone_, ast_value_factory->zone());
  zone_->ReleaseMemory();

  if (aborted) {
    // Prepare scope for use in the outer zone.
    zone_ = ast_value_factory->zone();
    variables_.Reset(ZoneAllocationPolicy(zone_));
    if (!IsArrowFunction(function_kind_)) {
      has_simple_parameters_ = true;
      DeclareDefaultFunctionVariables(ast_value_factory);
    }
  } else {
    // Make sure this scope isn't used for allocation anymore.
    zone_ = nullptr;
    variables_.Invalidate();
  }

#ifdef DEBUG
  needs_migration_ = false;
  is_being_lazily_parsed_ = false;
#endif

  was_lazily_parsed_ = !aborted;
}

bool Scope::IsSkippableFunctionScope() {
  // Lazy non-arrow function scopes are skippable. Lazy functions are exactly
  // those Scopes which have their own PreparseDataBuilder object. This
  // logic ensures that the scope allocation data is consistent with the
  // skippable function data (both agree on where the lazy function boundaries
  // are).
  if (!is_function_scope()) return false;
  DeclarationScope* declaration_scope = AsDeclarationScope();
  return !declaration_scope->is_arrow_scope() &&
         declaration_scope->preparse_data_builder() != nullptr;
}

void Scope::SavePreparseData(Parser* parser) {
  this->ForEach([parser](Scope* scope) {
    if (scope->IsSkippableFunctionScope()) {
      scope->AsDeclarationScope()->SavePreparseDataForDeclarationScope(parser);
    }
    return Iteration::kDescend;
  });
}

void DeclarationScope::SavePreparseDataForDeclarationScope(Parser* parser) {
  if (preparse_data_builder_ == nullptr) return;
  preparse_data_builder_->SaveScopeAllocationData(this, parser);
}

void DeclarationScope::AnalyzePartially(Parser* parser,
                                        AstNodeFactory* ast_node_factory) {
  DCHECK(!force_eager_compilation_);
  UnresolvedList new_unresolved_list;
  if (!IsArrowFunction(function_kind_) &&
      (!outer_scope_->is_script_scope() ||
       (preparse_data_builder_ != nullptr &&
        preparse_data_builder_->HasInnerFunctions()))) {
    // Try to resolve unresolved variables for this Scope and migrate those
    // which cannot be resolved inside. It doesn't make sense to try to resolve
    // them in the outer Scopes here, because they are incomplete.
    Scope::AnalyzePartially(this, ast_node_factory, &new_unresolved_list);

    // Migrate function_ to the right Zone.
    if (function_ != nullptr) {
      function_ = ast_node_factory->CopyVariable(function_);
    }

    SavePreparseData(parser);
  }

#ifdef DEBUG
  if (FLAG_print_scopes) {
    PrintF("Inner function scope:\n");
    Print();
  }
#endif

  ResetAfterPreparsing(ast_node_factory->ast_value_factory(), false);

  unresolved_list_ = std::move(new_unresolved_list);
}

#ifdef DEBUG
namespace {

const char* Header(ScopeType scope_type, FunctionKind function_kind,
                   bool is_declaration_scope) {
  switch (scope_type) {
    case EVAL_SCOPE: return "eval";
    // TODO(adamk): Should we print concise method scopes specially?
    case FUNCTION_SCOPE:
      if (IsGeneratorFunction(function_kind)) return "function*";
      if (IsAsyncFunction(function_kind)) return "async function";
      if (IsArrowFunction(function_kind)) return "arrow";
      return "function";
    case MODULE_SCOPE: return "module";
    case SCRIPT_SCOPE: return "global";
    case CATCH_SCOPE: return "catch";
    case BLOCK_SCOPE: return is_declaration_scope ? "varblock" : "block";
    case CLASS_SCOPE:
      return "class";
    case WITH_SCOPE: return "with";
  }
  UNREACHABLE();
}

void Indent(int n, const char* str) { PrintF("%*s%s", n, "", str); }

void PrintName(const AstRawString* name) {
  PrintF("%.*s", name->length(), name->raw_data());
}

void PrintLocation(Variable* var) {
  switch (var->location()) {
    case VariableLocation::UNALLOCATED:
      break;
    case VariableLocation::PARAMETER:
      PrintF("parameter[%d]", var->index());
      break;
    case VariableLocation::LOCAL:
      PrintF("local[%d]", var->index());
      break;
    case VariableLocation::CONTEXT:
      PrintF("context[%d]", var->index());
      break;
    case VariableLocation::LOOKUP:
      PrintF("lookup");
      break;
    case VariableLocation::MODULE:
      PrintF("module");
      break;
  }
}

void PrintVar(int indent, Variable* var) {
  Indent(indent, VariableMode2String(var->mode()));
  PrintF(" ");
  if (var->raw_name()->IsEmpty())
    PrintF(".%p", reinterpret_cast<void*>(var));
  else
    PrintName(var->raw_name());
  PrintF(";  // (%p) ", reinterpret_cast<void*>(var));
  PrintLocation(var);
  bool comma = !var->IsUnallocated();
  if (var->has_forced_context_allocation()) {
    if (comma) PrintF(", ");
    PrintF("forced context allocation");
    comma = true;
  }
  if (var->maybe_assigned() == kNotAssigned) {
    if (comma) PrintF(", ");
    PrintF("never assigned");
    comma = true;
  }
  if (var->initialization_flag() == kNeedsInitialization &&
      !var->binding_needs_init()) {
    if (comma) PrintF(", ");
    PrintF("hole initialization elided");
  }
  PrintF("\n");
}

void PrintMap(int indent, const char* label, VariableMap* map, bool locals,
              Variable* function_var) {
  bool printed_label = false;
  for (VariableMap::Entry* p = map->Start(); p != nullptr; p = map->Next(p)) {
    Variable* var = reinterpret_cast<Variable*>(p->value);
    if (var == function_var) continue;
    bool local = !IsDynamicVariableMode(var->mode());
    if ((locals ? local : !local) &&
        (var->is_used() || !var->IsUnallocated())) {
      if (!printed_label) {
        Indent(indent, label);
        printed_label = true;
      }
      PrintVar(indent, var);
    }
  }
}

}  // anonymous namespace

void DeclarationScope::PrintParameters() {
  PrintF(" (");
  for (int i = 0; i < params_.length(); i++) {
    if (i > 0) PrintF(", ");
    const AstRawString* name = params_[i]->raw_name();
    if (name->IsEmpty()) {
      PrintF(".%p", reinterpret_cast<void*>(params_[i]));
    } else {
      PrintName(name);
    }
  }
  PrintF(")");
}

void Scope::Print(int n) {
  int n0 = (n > 0 ? n : 0);
  int n1 = n0 + 2;  // indentation

  // Print header.
  FunctionKind function_kind = is_function_scope()
                                   ? AsDeclarationScope()->function_kind()
                                   : kNormalFunction;
  Indent(n0, Header(scope_type_, function_kind, is_declaration_scope()));
  if (scope_name_ != nullptr && !scope_name_->IsEmpty()) {
    PrintF(" ");
    PrintName(scope_name_);
  }

  // Print parameters, if any.
  Variable* function = nullptr;
  if (is_function_scope()) {
    AsDeclarationScope()->PrintParameters();
    function = AsDeclarationScope()->function_var();
  }

  PrintF(" { // (%p) (%d, %d)\n", reinterpret_cast<void*>(this),
         start_position(), end_position());
  if (is_hidden()) {
    Indent(n1, "// is hidden\n");
  }

  // Function name, if any (named function literals, only).
  if (function != nullptr) {
    Indent(n1, "// (local) function name: ");
    PrintName(function->raw_name());
    PrintF("\n");
  }

  // Scope info.
  if (is_strict(language_mode())) {
    Indent(n1, "// strict mode scope\n");
  }
  if (IsAsmModule()) Indent(n1, "// scope is an asm module\n");
  if (is_declaration_scope() && AsDeclarationScope()->calls_sloppy_eval()) {
    Indent(n1, "// scope calls sloppy 'eval'\n");
  }
  if (is_declaration_scope() && AsDeclarationScope()->NeedsHomeObject()) {
    Indent(n1, "// scope needs home object\n");
  }
  if (inner_scope_calls_eval_) Indent(n1, "// inner scope calls 'eval'\n");
  if (is_declaration_scope()) {
    DeclarationScope* scope = AsDeclarationScope();
    if (scope->was_lazily_parsed()) Indent(n1, "// lazily parsed\n");
    if (scope->ShouldEagerCompile()) Indent(n1, "// will be compiled\n");
  }
  if (num_stack_slots_ > 0) {
    Indent(n1, "// ");
    PrintF("%d stack slots\n", num_stack_slots_);
  }
  if (num_heap_slots_ > 0) {
    Indent(n1, "// ");
    PrintF("%d heap slots\n", num_heap_slots_);
  }

  // Print locals.
  if (function != nullptr) {
    Indent(n1, "// function var:\n");
    PrintVar(n1, function);
  }

  // Print temporaries.
  {
    bool printed_header = false;
    for (Variable* local : locals_) {
      if (local->mode() != VariableMode::kTemporary) continue;
      if (!printed_header) {
        printed_header = true;
        Indent(n1, "// temporary vars:\n");
      }
      PrintVar(n1, local);
    }
  }

  if (variables_.occupancy() > 0) {
    PrintMap(n1, "// local vars:\n", &variables_, true, function);
    PrintMap(n1, "// dynamic vars:\n", &variables_, false, function);
  }

  if (is_class_scope()) {
    ClassScope* class_scope = AsClassScope();
    if (class_scope->rare_data_ != nullptr) {
      PrintMap(n1, "// private name vars:\n",
               &(class_scope->rare_data_->private_name_map), true, function);
      Variable* brand = class_scope->brand();
      if (brand != nullptr) {
        Indent(n1, "// brand var:\n");
        PrintVar(n1, brand);
      }
    }
  }

  // Print inner scopes (disable by providing negative n).
  if (n >= 0) {
    for (Scope* scope = inner_scope_; scope != nullptr;
         scope = scope->sibling_) {
      PrintF("\n");
      scope->Print(n1);
    }
  }

  Indent(n0, "}\n");
}

void Scope::CheckScopePositions() {
  this->ForEach([](Scope* scope) {
    // Visible leaf scopes must have real positions.
    if (!scope->is_hidden() && scope->inner_scope_ == nullptr) {
      DCHECK_NE(kNoSourcePosition, scope->start_position());
      DCHECK_NE(kNoSourcePosition, scope->end_position());
    }
    return Iteration::kDescend;
  });
}

void Scope::CheckZones() {
  DCHECK(!needs_migration_);
  this->ForEach([](Scope* scope) {
    if (WasLazilyParsed(scope)) {
      DCHECK_NULL(scope->zone());
      DCHECK_NULL(scope->inner_scope_);
      return Iteration::kContinue;
    }
    return Iteration::kDescend;
  });
}
#endif  // DEBUG

Variable* Scope::NonLocal(const AstRawString* name, VariableMode mode) {
  // Declare a new non-local.
  DCHECK(IsDynamicVariableMode(mode));
  bool was_added;
  Variable* var =
      variables_.Declare(zone(), this, name, mode, NORMAL_VARIABLE,
                         kCreatedInitialized, kNotAssigned, &was_added);
  // Allocate it by giving it a dynamic lookup.
  var->AllocateTo(VariableLocation::LOOKUP, -1);
  return var;
}

// static
template <Scope::ScopeLookupMode mode>
Variable* Scope::Lookup(VariableProxy* proxy, Scope* scope,
                        Scope* outer_scope_end, Scope* entry_point,
                        bool force_context_allocation) {
  if (mode == kDeserializedScope) {
    Variable* var = entry_point->variables_.Lookup(proxy->raw_name());
    if (var != nullptr) return var;
  }

  while (true) {
    DCHECK_IMPLIES(mode == kParsedScope, !scope->is_debug_evaluate_scope_);
    // Short-cut: whenever we find a debug-evaluate scope, just look everything
    // up dynamically. Debug-evaluate doesn't properly create scope info for the
    // lookups it does. It may not have a valid 'this' declaration, and anything
    // accessed through debug-evaluate might invalidly resolve to
    // stack-allocated variables.
    // TODO(yangguo): Remove once debug-evaluate creates proper ScopeInfo for
    // the scopes in which it's evaluating.
    if (mode == kDeserializedScope &&
        V8_UNLIKELY(scope->is_debug_evaluate_scope_)) {
      return entry_point->NonLocal(proxy->raw_name(), VariableMode::kDynamic);
    }

    // Try to find the variable in this scope.
    Variable* var = mode == kParsedScope ? scope->LookupLocal(proxy->raw_name())
                                         : scope->LookupInScopeInfo(
                                               proxy->raw_name(), entry_point);

    // We found a variable and we are done. (Even if there is an 'eval' in this
    // scope which introduces the same variable again, the resulting variable
    // remains the same.)
    if (var != nullptr) {
      if (mode == kParsedScope && force_context_allocation &&
          !var->is_dynamic()) {
        var->ForceContextAllocation();
      }
      return var;
    }

    if (scope->outer_scope_ == outer_scope_end) break;

    DCHECK(!scope->is_script_scope());
    if (V8_UNLIKELY(scope->is_with_scope())) {
      return LookupWith(proxy, scope, outer_scope_end, entry_point,
                        force_context_allocation);
    }
    if (V8_UNLIKELY(scope->is_declaration_scope() &&
                    scope->AsDeclarationScope()->calls_sloppy_eval())) {
      return LookupSloppyEval(proxy, scope, outer_scope_end, entry_point,
                              force_context_allocation);
    }

    force_context_allocation |= scope->is_function_scope();
    scope = scope->outer_scope_;
    // TODO(verwaest): Separate through AnalyzePartially.
    if (mode == kParsedScope && !scope->scope_info_.is_null()) {
      return Lookup<kDeserializedScope>(proxy, scope, outer_scope_end, scope);
    }
  }

  // We may just be trying to find all free variables. In that case, don't
  // declare them in the outer scope.
  // TODO(marja): Separate Lookup for preparsed scopes better.
  if (mode == kParsedScope && !scope->is_script_scope()) {
    return nullptr;
  }

  // No binding has been found. Declare a variable on the global object.
  return scope->AsDeclarationScope()->DeclareDynamicGlobal(
      proxy->raw_name(), NORMAL_VARIABLE,
      mode == kDeserializedScope ? entry_point : scope);
}

template Variable* Scope::Lookup<Scope::kParsedScope>(
    VariableProxy* proxy, Scope* scope, Scope* outer_scope_end,
    Scope* entry_point, bool force_context_allocation);
template Variable* Scope::Lookup<Scope::kDeserializedScope>(
    VariableProxy* proxy, Scope* scope, Scope* outer_scope_end,
    Scope* entry_point, bool force_context_allocation);

Variable* Scope::LookupWith(VariableProxy* proxy, Scope* scope,
                            Scope* outer_scope_end, Scope* entry_point,
                            bool force_context_allocation) {
  DCHECK(scope->is_with_scope());

  Variable* var =
      scope->outer_scope_->scope_info_.is_null()
          ? Lookup<kParsedScope>(proxy, scope->outer_scope_, outer_scope_end,
                                 nullptr, force_context_allocation)
          : Lookup<kDeserializedScope>(proxy, scope->outer_scope_,
                                       outer_scope_end, entry_point);

  if (var == nullptr) return var;

  // The current scope is a with scope, so the variable binding can not be
  // statically resolved. However, note that it was necessary to do a lookup
  // in the outer scope anyway, because if a binding exists in an outer
  // scope, the associated variable has to be marked as potentially being
  // accessed from inside of an inner with scope (the property may not be in
  // the 'with' object).
  if (!var->is_dynamic() && var->IsUnallocated()) {
    DCHECK(!scope->already_resolved_);
    var->set_is_used();
    var->ForceContextAllocation();
    if (proxy->is_assigned()) var->SetMaybeAssigned();
  }
  if (entry_point != nullptr) entry_point->variables_.Remove(var);
  Scope* target = entry_point == nullptr ? scope : entry_point;
  Variable* dynamic =
      target->NonLocal(proxy->raw_name(), VariableMode::kDynamic);
  dynamic->set_local_if_not_shadowed(var);
  return dynamic;
}

Variable* Scope::LookupSloppyEval(VariableProxy* proxy, Scope* scope,
                                  Scope* outer_scope_end, Scope* entry_point,
                                  bool force_context_allocation) {
  DCHECK(scope->is_declaration_scope() &&
         scope->AsDeclarationScope()->calls_sloppy_eval());

  // If we're compiling eval, it's possible that the outer scope is the first
  // ScopeInfo-backed scope.
  Scope* entry = entry_point == nullptr ? scope->outer_scope_ : entry_point;
  Variable* var =
      scope->outer_scope_->scope_info_.is_null()
          ? Lookup<kParsedScope>(proxy, scope->outer_scope_, outer_scope_end,
                                 nullptr, force_context_allocation)
          : Lookup<kDeserializedScope>(proxy, scope->outer_scope_,
                                       outer_scope_end, entry);
  if (var == nullptr) return var;

  // A variable binding may have been found in an outer scope, but the current
  // scope makes a sloppy 'eval' call, so the found variable may not be the
  // correct one (the 'eval' may introduce a binding with the same name). In
  // that case, change the lookup result to reflect this situation. Only
  // scopes that can host var bindings (declaration scopes) need be considered
  // here (this excludes block and catch scopes), and variable lookups at
  // script scope are always dynamic.
  if (var->IsGlobalObjectProperty()) {
    Scope* target = entry_point == nullptr ? scope : entry_point;
    var = target->NonLocal(proxy->raw_name(), VariableMode::kDynamicGlobal);
  }

  if (var->is_dynamic()) return var;

  Variable* invalidated = var;
  if (entry_point != nullptr) entry_point->variables_.Remove(invalidated);

  Scope* target = entry_point == nullptr ? scope : entry_point;
  var = target->NonLocal(proxy->raw_name(), VariableMode::kDynamicLocal);
  var->set_local_if_not_shadowed(invalidated);

  return var;
}

void Scope::ResolveVariable(ParseInfo* info, VariableProxy* proxy) {
  DCHECK(info->script_scope()->is_script_scope());
  DCHECK(!proxy->is_resolved());
  Variable* var = Lookup<kParsedScope>(proxy, this, nullptr);
  DCHECK_NOT_NULL(var);
  ResolveTo(info, proxy, var);
}

namespace {

void SetNeedsHoleCheck(Variable* var, VariableProxy* proxy) {
  proxy->set_needs_hole_check();
  var->ForceHoleInitialization();
}

void UpdateNeedsHoleCheck(Variable* var, VariableProxy* proxy, Scope* scope) {
  if (var->mode() == VariableMode::kDynamicLocal) {
    // Dynamically introduced variables never need a hole check (since they're
    // VariableMode::kVar bindings, either from var or function declarations),
    // but the variable they shadow might need a hole check, which we want to do
    // if we decide that no shadowing variable was dynamically introoduced.
    DCHECK_EQ(kCreatedInitialized, var->initialization_flag());
    return UpdateNeedsHoleCheck(var->local_if_not_shadowed(), proxy, scope);
  }

  if (var->initialization_flag() == kCreatedInitialized) return;

  // It's impossible to eliminate module import hole checks here, because it's
  // unknown at compilation time whether the binding referred to in the
  // exporting module itself requires hole checks.
  if (var->location() == VariableLocation::MODULE && !var->IsExport()) {
    return SetNeedsHoleCheck(var, proxy);
  }

  // Check if the binding really needs an initialization check. The check
  // can be skipped in the following situation: we have a VariableMode::kLet or
  // VariableMode::kConst binding, both the Variable and the VariableProxy have
  // the same declaration scope (i.e. they are both in global code, in the same
  // function or in the same eval code), the VariableProxy is in the source
  // physically located after the initializer of the variable, and that the
  // initializer cannot be skipped due to a nonlinear scope.
  //
  // The condition on the closure scopes is a conservative check for
  // nested functions that access a binding and are called before the
  // binding is initialized:
  //   function() { f(); let x = 1; function f() { x = 2; } }
  //
  // The check cannot be skipped on non-linear scopes, namely switch
  // scopes, to ensure tests are done in cases like the following:
  //   switch (1) { case 0: let x = 2; case 1: f(x); }
  // The scope of the variable needs to be checked, in case the use is
  // in a sub-block which may be linear.
  if (var->scope()->GetClosureScope() != scope->GetClosureScope()) {
    return SetNeedsHoleCheck(var, proxy);
  }

  // We should always have valid source positions.
  DCHECK_NE(var->initializer_position(), kNoSourcePosition);
  DCHECK_NE(proxy->position(), kNoSourcePosition);

  if (var->scope()->is_nonlinear() ||
      var->initializer_position() >= proxy->position()) {
    return SetNeedsHoleCheck(var, proxy);
  }
}

}  // anonymous namespace

void Scope::ResolveTo(ParseInfo* info, VariableProxy* proxy, Variable* var) {
  DCHECK_NOT_NULL(var);
  UpdateNeedsHoleCheck(var, proxy, this);
  proxy->BindTo(var);
}

void Scope::ResolvePreparsedVariable(VariableProxy* proxy, Scope* scope,
                                     Scope* end) {
  // Resolve the variable in all parsed scopes to force context allocation.
  for (; scope != end; scope = scope->outer_scope_) {
    Variable* var = scope->LookupLocal(proxy->raw_name());
    if (var != nullptr) {
      var->set_is_used();
      if (!var->is_dynamic()) {
        var->ForceContextAllocation();
        if (proxy->is_assigned()) var->SetMaybeAssigned();
        return;
      }
    }
  }
}

bool Scope::ResolveVariablesRecursively(ParseInfo* info) {
  DCHECK(info->script_scope()->is_script_scope());
  // Lazy parsed declaration scopes are already partially analyzed. If there are
  // unresolved references remaining, they just need to be resolved in outer
  // scopes.
  if (WasLazilyParsed(this)) {
    DCHECK_EQ(variables_.occupancy(), 0);
    Scope* end = info->scope();
    // Resolve in all parsed scopes except for the script scope.
    if (!end->is_script_scope()) end = end->outer_scope();

    for (VariableProxy* proxy : unresolved_list_) {
      ResolvePreparsedVariable(proxy, outer_scope(), end);
    }
  } else {
    // Resolve unresolved variables for this scope.
    for (VariableProxy* proxy : unresolved_list_) {
      ResolveVariable(info, proxy);
    }

    // Resolve unresolved variables for inner scopes.
    for (Scope* scope = inner_scope_; scope != nullptr;
         scope = scope->sibling_) {
      if (!scope->ResolveVariablesRecursively(info)) return false;
    }
  }
  return true;
}

bool Scope::MustAllocate(Variable* var) {
  DCHECK(var->location() != VariableLocation::MODULE);
  // Give var a read/write use if there is a chance it might be accessed
  // via an eval() call.  This is only possible if the variable has a
  // visible name.
  if (!var->raw_name()->IsEmpty() &&
      (inner_scope_calls_eval_ || is_catch_scope() || is_script_scope())) {
    var->set_is_used();
    if (inner_scope_calls_eval_) var->SetMaybeAssigned();
  }
  DCHECK(!var->has_forced_context_allocation() || var->is_used());
  // Global variables do not need to be allocated.
  return !var->IsGlobalObjectProperty() && var->is_used();
}


bool Scope::MustAllocateInContext(Variable* var) {
  // If var is accessed from an inner scope, or if there is a possibility
  // that it might be accessed from the current or an inner scope (through
  // an eval() call or a runtime with lookup), it must be allocated in the
  // context.
  //
  // Temporary variables are always stack-allocated.  Catch-bound variables are
  // always context-allocated.
  VariableMode mode = var->mode();
  if (mode == VariableMode::kTemporary) return false;
  if (is_catch_scope()) return true;
  if (is_script_scope() || is_eval_scope()) {
    if (IsLexicalVariableMode(mode) ||
        IsPrivateMethodOrAccessorVariableMode(mode)) {
      return true;
    }
  }
  return var->has_forced_context_allocation() || inner_scope_calls_eval_;
}


void Scope::AllocateStackSlot(Variable* var) {
  if (is_block_scope()) {
    outer_scope()->GetDeclarationScope()->AllocateStackSlot(var);
  } else {
    var->AllocateTo(VariableLocation::LOCAL, num_stack_slots_++);
  }
}


void Scope::AllocateHeapSlot(Variable* var) {
  var->AllocateTo(VariableLocation::CONTEXT, num_heap_slots_++);
}

void DeclarationScope::AllocateParameterLocals() {
  DCHECK(is_function_scope());

  bool has_mapped_arguments = false;
  if (arguments_ != nullptr) {
    DCHECK(!is_arrow_scope());
    if (MustAllocate(arguments_) && !has_arguments_parameter_) {
      // 'arguments' is used and does not refer to a function
      // parameter of the same name. If the arguments object
      // aliases formal parameters, we conservatively allocate
      // them specially in the loop below.
      has_mapped_arguments =
          GetArgumentsType() == CreateArgumentsType::kMappedArguments;
    } else {
      // 'arguments' is unused. Tell the code generator that it does not need to
      // allocate the arguments object by nulling out arguments_.
      arguments_ = nullptr;
    }
  }

  // The same parameter may occur multiple times in the parameters_ list.
  // If it does, and if it is not copied into the context object, it must
  // receive the highest parameter index for that parameter; thus iteration
  // order is relevant!
  for (int i = num_parameters() - 1; i >= 0; --i) {
    Variable* var = params_[i];
    DCHECK_NOT_NULL(var);
    DCHECK(!has_rest_ || var != rest_parameter());
    DCHECK_EQ(this, var->scope());
    if (has_mapped_arguments) {
      var->set_is_used();
      var->SetMaybeAssigned();
      var->ForceContextAllocation();
    }
    AllocateParameter(var, i);
  }
}

void DeclarationScope::AllocateParameter(Variable* var, int index) {
  if (!MustAllocate(var)) return;
  if (has_forced_context_allocation_for_parameters() ||
      MustAllocateInContext(var)) {
    DCHECK(var->IsUnallocated() || var->IsContextSlot());
    if (var->IsUnallocated()) AllocateHeapSlot(var);
  } else {
    DCHECK(var->IsUnallocated() || var->IsParameter());
    if (var->IsUnallocated()) {
      var->AllocateTo(VariableLocation::PARAMETER, index);
    }
  }
}

void DeclarationScope::AllocateReceiver() {
  if (!has_this_declaration()) return;
  DCHECK_NOT_NULL(receiver());
  DCHECK_EQ(receiver()->scope(), this);
  AllocateParameter(receiver(), -1);
}

void Scope::AllocateNonParameterLocal(Variable* var) {
  DCHECK_EQ(var->scope(), this);
  if (var->IsUnallocated() && MustAllocate(var)) {
    if (MustAllocateInContext(var)) {
      AllocateHeapSlot(var);
      DCHECK_IMPLIES(is_catch_scope(),
                     var->index() == Context::THROWN_OBJECT_INDEX);
    } else {
      AllocateStackSlot(var);
    }
  }
}

void Scope::AllocateNonParameterLocalsAndDeclaredGlobals() {
  for (Variable* local : locals_) {
    AllocateNonParameterLocal(local);
  }

  if (is_declaration_scope()) {
    AsDeclarationScope()->AllocateLocals();
  }
}

void DeclarationScope::AllocateLocals() {
  // For now, function_ must be allocated at the very end.  If it gets
  // allocated in the context, it must be the last slot in the context,
  // because of the current ScopeInfo implementation (see
  // ScopeInfo::ScopeInfo(FunctionScope* scope) constructor).
  if (function_ != nullptr && MustAllocate(function_)) {
    AllocateNonParameterLocal(function_);
  } else {
    function_ = nullptr;
  }

  DCHECK(!has_rest_ || !MustAllocate(rest_parameter()) ||
         !rest_parameter()->IsUnallocated());

  if (new_target_ != nullptr && !MustAllocate(new_target_)) {
    new_target_ = nullptr;
  }

  NullifyRareVariableIf(RareVariable::kThisFunction,
                        [=](Variable* var) { return !MustAllocate(var); });
}

void ModuleScope::AllocateModuleVariables() {
  for (const auto& it : module()->regular_imports()) {
    Variable* var = LookupLocal(it.first);
    var->AllocateTo(VariableLocation::MODULE, it.second->cell_index);
    DCHECK(!var->IsExport());
  }

  for (const auto& it : module()->regular_exports()) {
    Variable* var = LookupLocal(it.first);
    var->AllocateTo(VariableLocation::MODULE, it.second->cell_index);
    DCHECK(var->IsExport());
  }
}

void Scope::AllocateVariablesRecursively() {
  this->ForEach([](Scope* scope) -> Iteration {
    DCHECK(!scope->already_resolved_);
    if (WasLazilyParsed(scope)) return Iteration::kContinue;
    DCHECK_EQ(Context::MIN_CONTEXT_SLOTS, scope->num_heap_slots_);

    // Allocate variables for this scope.
    // Parameters must be allocated first, if any.
    if (scope->is_declaration_scope()) {
      if (scope->is_function_scope()) {
        scope->AsDeclarationScope()->AllocateParameterLocals();
      }
      scope->AsDeclarationScope()->AllocateReceiver();
    }
    scope->AllocateNonParameterLocalsAndDeclaredGlobals();

    // Force allocation of a context for this scope if necessary. For a 'with'
    // scope and for a function scope that makes an 'eval' call we need a
    // context, even if no local variables were statically allocated in the
    // scope. Likewise for modules and function scopes representing asm.js
    // modules. Also force a context, if the scope is stricter than the outer
    // scope.
    bool must_have_context =
        scope->is_with_scope() || scope->is_module_scope() ||
        scope->IsAsmModule() || scope->ForceContextForLanguageMode() ||
        (scope->is_function_scope() &&
         scope->AsDeclarationScope()->calls_sloppy_eval()) ||
        (scope->is_block_scope() && scope->is_declaration_scope() &&
         scope->AsDeclarationScope()->calls_sloppy_eval());

    // If we didn't allocate any locals in the local context, then we only
    // need the minimal number of slots if we must have a context.
    if (scope->num_heap_slots_ == Context::MIN_CONTEXT_SLOTS &&
        !must_have_context) {
      scope->num_heap_slots_ = 0;
    }

    // Allocation done.
    DCHECK(scope->num_heap_slots_ == 0 ||
           scope->num_heap_slots_ >= Context::MIN_CONTEXT_SLOTS);
    return Iteration::kDescend;
  });
}

void Scope::AllocateScopeInfosRecursively(Isolate* isolate,
                                          MaybeHandle<ScopeInfo> outer_scope) {
  DCHECK(scope_info_.is_null());
  MaybeHandle<ScopeInfo> next_outer_scope = outer_scope;

  if (NeedsScopeInfo()) {
    scope_info_ = ScopeInfo::Create(isolate, zone(), this, outer_scope);
    // The ScopeInfo chain should mirror the context chain, so we only link to
    // the next outer scope that needs a context.
    if (NeedsContext()) next_outer_scope = scope_info_;
  }

  // Allocate ScopeInfos for inner scopes.
  for (Scope* scope = inner_scope_; scope != nullptr; scope = scope->sibling_) {
    if (!scope->is_function_scope() ||
        scope->AsDeclarationScope()->ShouldEagerCompile()) {
      scope->AllocateScopeInfosRecursively(isolate, next_outer_scope);
    }
  }
}

// static
void DeclarationScope::AllocateScopeInfos(ParseInfo* info, Isolate* isolate) {
  DeclarationScope* scope = info->literal()->scope();
  if (!scope->scope_info_.is_null()) return;  // Allocated by outer function.

  MaybeHandle<ScopeInfo> outer_scope;
  if (scope->outer_scope_ != nullptr) {
    outer_scope = scope->outer_scope_->scope_info_;
  }

  scope->AllocateScopeInfosRecursively(isolate, outer_scope);

  // The debugger expects all shared function infos to contain a scope info.
  // Since the top-most scope will end up in a shared function info, make sure
  // it has one, even if it doesn't need a scope info.
  // TODO(jochen|yangguo): Remove this requirement.
  if (scope->scope_info_.is_null()) {
    scope->scope_info_ =
        ScopeInfo::Create(isolate, scope->zone(), scope, outer_scope);
  }

  // Ensuring that the outer script scope has a scope info avoids having
  // special case for native contexts vs other contexts.
  if (info->script_scope() && info->script_scope()->scope_info_.is_null()) {
    info->script_scope()->scope_info_ =
        handle(ScopeInfo::Empty(isolate), isolate);
  }
}

int Scope::ContextLocalCount() const {
  if (num_heap_slots() == 0) return 0;
  Variable* function =
      is_function_scope() ? AsDeclarationScope()->function_var() : nullptr;
  bool is_function_var_in_context =
      function != nullptr && function->IsContextSlot();
  return num_heap_slots() - Context::MIN_CONTEXT_SLOTS -
         (is_function_var_in_context ? 1 : 0);
}

bool IsComplementaryAccessorPair(VariableMode a, VariableMode b) {
  switch (a) {
    case VariableMode::kPrivateGetterOnly:
      return b == VariableMode::kPrivateSetterOnly;
    case VariableMode::kPrivateSetterOnly:
      return b == VariableMode::kPrivateGetterOnly;
    default:
      return false;
  }
}

Variable* ClassScope::DeclarePrivateName(const AstRawString* name,
                                         VariableMode mode, bool* was_added) {
  Variable* result = EnsureRareData()->private_name_map.Declare(
      zone(), this, name, mode, NORMAL_VARIABLE,
      InitializationFlag::kNeedsInitialization,
      MaybeAssignedFlag::kMaybeAssigned, was_added);
  if (*was_added) {
    locals_.Add(result);
  } else if (IsComplementaryAccessorPair(result->mode(), mode)) {
    *was_added = true;
    result->set_mode(VariableMode::kPrivateGetterAndSetter);
  }
  result->ForceContextAllocation();
  return result;
}

Variable* ClassScope::LookupLocalPrivateName(const AstRawString* name) {
  if (rare_data_ == nullptr) {
    return nullptr;
  }
  return rare_data_->private_name_map.Lookup(name);
}

UnresolvedList::Iterator ClassScope::GetUnresolvedPrivateNameTail() {
  if (rare_data_ == nullptr) {
    return UnresolvedList::Iterator();
  }
  return rare_data_->unresolved_private_names.end();
}

void ClassScope::ResetUnresolvedPrivateNameTail(UnresolvedList::Iterator tail) {
  if (rare_data_ == nullptr ||
      rare_data_->unresolved_private_names.end() == tail) {
    return;
  }

  bool tail_is_empty = tail == UnresolvedList::Iterator();
  if (tail_is_empty) {
    // If the saved tail is empty, the list used to be empty, so clear it.
    rare_data_->unresolved_private_names.Clear();
  } else {
    rare_data_->unresolved_private_names.Rewind(tail);
  }
}

void ClassScope::MigrateUnresolvedPrivateNameTail(
    AstNodeFactory* ast_node_factory, UnresolvedList::Iterator tail) {
  if (rare_data_ == nullptr ||
      rare_data_->unresolved_private_names.end() == tail) {
    return;
  }
  UnresolvedList migrated_names;

  // If the saved tail is empty, the list used to be empty, so we should
  // migrate everything after the head.
  bool tail_is_empty = tail == UnresolvedList::Iterator();
  UnresolvedList::Iterator it =
      tail_is_empty ? rare_data_->unresolved_private_names.begin() : tail;

  for (; it != rare_data_->unresolved_private_names.end(); ++it) {
    VariableProxy* proxy = *it;
    VariableProxy* copy = ast_node_factory->CopyVariableProxy(proxy);
    migrated_names.Add(copy);
  }

  // Replace with the migrated copies.
  if (tail_is_empty) {
    rare_data_->unresolved_private_names.Clear();
  } else {
    rare_data_->unresolved_private_names.Rewind(tail);
  }
  rare_data_->unresolved_private_names.Append(std::move(migrated_names));
}

void ClassScope::AddUnresolvedPrivateName(VariableProxy* proxy) {
  // During a reparse, already_resolved_ may be true here, because
  // the class scope is deserialized while the function scope inside may
  // be new.
  DCHECK(!proxy->is_resolved());
  DCHECK(proxy->IsPrivateName());
  EnsureRareData()->unresolved_private_names.Add(proxy);
}

Variable* ClassScope::LookupPrivateNameInScopeInfo(const AstRawString* name) {
  DCHECK(!scope_info_.is_null());
  DCHECK_NULL(LookupLocalPrivateName(name));
  DisallowHeapAllocation no_gc;

  String name_handle = *name->string();
  VariableMode mode;
  InitializationFlag init_flag;
  MaybeAssignedFlag maybe_assigned_flag;
  int index = ScopeInfo::ContextSlotIndex(*scope_info_, name_handle, &mode,
                                          &init_flag, &maybe_assigned_flag);
  if (index < 0) {
    return nullptr;
  }

  DCHECK(IsConstVariableMode(mode));
  DCHECK_EQ(init_flag, InitializationFlag::kNeedsInitialization);
  DCHECK_EQ(maybe_assigned_flag, MaybeAssignedFlag::kMaybeAssigned);

  // Add the found private name to the map to speed up subsequent
  // lookups for the same name.
  bool was_added;
  Variable* var = DeclarePrivateName(name, mode, &was_added);
  DCHECK(was_added);
  var->AllocateTo(VariableLocation::CONTEXT, index);
  return var;
}

Variable* ClassScope::LookupPrivateName(VariableProxy* proxy) {
  DCHECK(!proxy->is_resolved());

  for (Scope* scope = this; !scope->is_script_scope();
       scope = scope->outer_scope_) {
    if (!scope->is_class_scope()) continue;  // Only search in class scopes
    ClassScope* class_scope = scope->AsClassScope();
    // Try finding it in the private name map first, if it can't be found,
    // try the deseralized scope info.
    Variable* var = class_scope->LookupLocalPrivateName(proxy->raw_name());
    if (var == nullptr && !class_scope->scope_info_.is_null()) {
      var = class_scope->LookupPrivateNameInScopeInfo(proxy->raw_name());
    }
    return var;
  }
  return nullptr;
}

bool ClassScope::ResolvePrivateNames(ParseInfo* info) {
  if (rare_data_ == nullptr ||
      rare_data_->unresolved_private_names.is_empty()) {
    return true;
  }

  UnresolvedList& list = rare_data_->unresolved_private_names;
  for (VariableProxy* proxy : list) {
    Variable* var = LookupPrivateName(proxy);
    if (var == nullptr) {
      Scanner::Location loc = proxy->location();
      info->pending_error_handler()->ReportMessageAt(
          loc.beg_pos, loc.end_pos,
          MessageTemplate::kInvalidPrivateFieldResolution, proxy->raw_name());
      return false;
    } else {
      var->set_is_used();
      proxy->BindTo(var);
    }
  }

  // By now all unresolved private names should be resolved so
  // clear the list.
  list.Clear();
  return true;
}

VariableProxy* ClassScope::ResolvePrivateNamesPartially() {
  if (rare_data_ == nullptr ||
      rare_data_->unresolved_private_names.is_empty()) {
    return nullptr;
  }

  ClassScope* outer_class_scope =
      outer_scope_ == nullptr ? nullptr : outer_scope_->GetClassScope();
  UnresolvedList& unresolved = rare_data_->unresolved_private_names;
  bool has_private_names = rare_data_->private_name_map.capacity() > 0;

  // If the class itself does not have private names, nor does it have
  // an outer class scope, then we are certain any private name access
  // inside cannot be resolved.
  if (!has_private_names && outer_class_scope == nullptr &&
      !unresolved.is_empty()) {
    return unresolved.first();
  }

  for (VariableProxy* proxy = unresolved.first(); proxy != nullptr;) {
    DCHECK(proxy->IsPrivateName());
    VariableProxy* next = proxy->next_unresolved();
    unresolved.Remove(proxy);
    Variable* var = nullptr;

    // If we can find private name in the current class scope, we can bind
    // them immediately because it's going to shadow any outer private names.
    if (has_private_names) {
      var = LookupLocalPrivateName(proxy->raw_name());
      if (var != nullptr) {
        var->set_is_used();
        proxy->BindTo(var);
      }
    }

    // If the current scope does not have declared private names,
    // try looking from the outer class scope later.
    if (var == nullptr) {
      // There's no outer class scope so we are certain that the variable
      // cannot be resolved later.
      if (outer_class_scope == nullptr) {
        return proxy;
      }

      // The private name may be found later in the outer class scope,
      // so push it to the outer sopce.
      outer_class_scope->AddUnresolvedPrivateName(proxy);
    }

    proxy = next;
  }

  DCHECK(unresolved.is_empty());
  return nullptr;
}

Variable* ClassScope::DeclareBrandVariable(AstValueFactory* ast_value_factory,
                                           int class_token_pos) {
  DCHECK_IMPLIES(rare_data_ != nullptr, rare_data_->brand == nullptr);
  bool was_added;
  Variable* brand = Declare(zone(), ast_value_factory->dot_brand_string(),
                            VariableMode::kConst, NORMAL_VARIABLE,
                            InitializationFlag::kNeedsInitialization,
                            MaybeAssignedFlag::kMaybeAssigned, &was_added);
  DCHECK(was_added);
  brand->ForceContextAllocation();
  brand->set_is_used();
  EnsureRareData()->brand = brand;
  brand->set_initializer_position(class_token_pos);
  return brand;
}

}  // namespace internal
}  // namespace v8
