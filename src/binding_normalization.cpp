/*
 * Copyright 2014 Google Inc. All rights reserved.
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

#define IN_FRUIT_CPP_FILE

#include <cstdlib>
#include <memory>
#include <vector>
#include <iostream>
#include <algorithm>
#include <fruit/impl/util/type_info.h>

#include <fruit/impl/storage/injector_storage.h>
#include <fruit/impl/storage/component_storage.h>
#include <fruit/impl/data_structures/semistatic_graph.templates.h>
#include <fruit/impl/meta/basics.h>
#include <fruit/impl/storage/normalized_component_storage.h>

using std::cout;
using std::endl;

using namespace fruit::impl;

namespace {

std::string multipleBindingsError(TypeId type) {
  return "Fatal injection error: the type " + type.type_info->name() + " was provided more than once, with different bindings.\n"
        + "This was not caught at compile time because at least one of the involved components bound this type but didn't expose it in the component signature.\n"
        + "If the type has a default constructor or an Inject annotation, this problem may arise even if this type is bound/provided by only one component (and then hidden), if this type is auto-injected in another component.\n"
        + "If the source of the problem is unclear, try exposing this type in all the component signatures where it's bound; if no component hides it this can't happen.\n";
}

auto typeInfoLessThanForMultibindings = [](const std::pair<TypeId, MultibindingData>& x,
                                           const std::pair<TypeId, MultibindingData>& y) {
  return x.first < y.first;
};

void printLazyComponentInstallationLoop(TypeId toplevel_component_fun_type_id,
                                        const std::vector<std::unique_ptr<LazyComponent>>& components_expansion_stack,
                                        const std::unique_ptr<LazyComponent>& last_component) {
  std::cerr << "Found a loop while expanding components passed to PartialComponent::install()." << std::endl;
  std::cerr << "Component installation trace (from top-level to the most deeply-nested):" << std::endl;
  std::cerr << std::string(toplevel_component_fun_type_id) << std::endl;
  for (const std::unique_ptr<LazyComponent>& component : components_expansion_stack) {
    if (*component == *last_component) {
      std::cerr << "<-- The loop starts here" << std::endl;
    }
    std::cerr << std::string(component->getFunTypeId()) << std::endl;
  }
  std::cerr << std::string(last_component->getFunTypeId()) << std::endl;
}

struct HashComponentPtr {
  inline std::size_t operator()(const LazyComponent* component) const {
    return component->hashCode();
  }
};

struct ComponentPtrEquals {
  inline bool operator()(const LazyComponent* component1, const LazyComponent* component2) const {
    return *component1 == *component2;
  }
};

struct HashComponentUniquePtr {
  inline std::size_t operator()(const std::unique_ptr<LazyComponent>& component) const {
    return component->hashCode();
  }
};

struct ComponentUniquePtrEquals {
  inline bool operator()(
      const std::unique_ptr<LazyComponent>& component1,
      const std::unique_ptr<LazyComponent>& component2) const {
    return *component1 == *component2;
  }
};


} // namespace

namespace fruit {
namespace impl {

std::vector<std::pair<TypeId, BindingData>>
BindingNormalization::normalizeBindings(
    std::vector<std::pair<TypeId, BindingData>>&& bindings_vector,
    FixedSizeAllocator::FixedSizeAllocatorData& fixed_size_allocator_data,
    std::vector<CompressedBinding>&& compressed_bindings_vector,
    const std::vector<std::pair<TypeId, MultibindingData>>& multibindings_vector,
    const std::vector<TypeId>& exposed_types,
    BindingCompressionInfoMap& bindingCompressionInfoMap) {
  HashMap<TypeId, BindingData> binding_data_map = createHashMap<TypeId, BindingData>(bindings_vector.size());

  for (auto& p : bindings_vector) {
    auto itr = binding_data_map.find(p.first);
    if (itr != binding_data_map.end()) {
      if (!(p.second == itr->second)) {
        std::cerr << multipleBindingsError(p.first) << std::endl;
        exit(1);
      }
      // Otherwise ok, duplicate but consistent binding.
      
    } else {
      // New binding, add it to the map.
      binding_data_map[p.first] = p.second;
    }
  }
  
  for (const auto& p : bindings_vector) {
    if (p.second.needsAllocation()) {
      fixed_size_allocator_data.addType(p.first);
    } else {
      fixed_size_allocator_data.addExternallyAllocatedType(p.first);
    }
  }
  
  // Remove duplicates from `compressedBindingsVector'.
  
  // CtypeId -> (ItypeId, bindingData)
  HashMap<TypeId, std::pair<TypeId, BindingData>> compressed_bindings_map =
      createHashMap<TypeId, std::pair<TypeId, BindingData>>(compressed_bindings_vector.size());
  
  // This also removes any duplicates. No need to check for multiple I->C, I2->C mappings, will filter these out later when 
  // considering deps.
  for (CompressedBinding& compressed_binding : compressed_bindings_vector) {
    compressed_bindings_map[compressed_binding.class_id] = {compressed_binding.interface_id, compressed_binding.binding_data};
  }
  
  // We can't compress the binding if C is a dep of a multibinding.
  for (auto p : multibindings_vector) {
    const BindingDeps* deps = p.second.deps;
    if (deps != nullptr) {
      for (std::size_t i = 0; i < deps->num_deps; ++i) {
        compressed_bindings_map.erase(deps->deps[i]);
#ifdef FRUIT_EXTRA_DEBUG
        std::cout << "InjectorStorage: ignoring compressed binding for " << deps->deps[i] << " because it's a dep of a multibinding." << std::endl;
#endif
      }
    }
  }
  
  // We can't compress the binding if C is an exposed type (but I is likely to be exposed instead).
  for (TypeId type : exposed_types) {
    compressed_bindings_map.erase(type);
#ifdef FRUIT_EXTRA_DEBUG
    std::cout << "InjectorStorage: ignoring compressed binding for " << type << " because it's an exposed type." << std::endl;
#endif
  }
  
  // We can't compress the binding if some type X depends on C and X!=I.
  for (auto& p : binding_data_map) {
    TypeId x_id = p.first;
    BindingData binding_data = p.second;
    if (!binding_data.isCreated()) {
      for (std::size_t i = 0; i < binding_data.getDeps()->num_deps; ++i) {
        TypeId c_id = binding_data.getDeps()->deps[i];
        auto itr = compressed_bindings_map.find(c_id);
        if (itr != compressed_bindings_map.end() && itr->second.first != x_id) {
          compressed_bindings_map.erase(itr);
#ifdef FRUIT_EXTRA_DEBUG
          std::cout << "InjectorStorage: ignoring compressed binding for " << c_id << " because the type " <<  x_id << " depends on it." << std::endl;
#endif
        }
      }
    }
  }
  
  // Two pairs of compressible bindings (I->C) and (C->X) can not exist (the C of a compressible binding is always bound either
  // using constructor binding or provider binding, it can't be a binding itself). So no need to check for that.
  
  bindingCompressionInfoMap = 
      createHashMap<TypeId, BindingNormalization::BindingCompressionInfo>(compressed_bindings_map.size());
  
  // Now perform the binding compression.
  for (auto& p : compressed_bindings_map) {
    TypeId c_id = p.first;
    TypeId i_id = p.second.first;
    BindingData binding_data = p.second.second;
    auto i_binding_data = binding_data_map.find(i_id);
    auto c_binding_data = binding_data_map.find(c_id);
    FruitAssert(i_binding_data != binding_data_map.end());
    FruitAssert(c_binding_data != binding_data_map.end());
    bindingCompressionInfoMap[c_id] = BindingCompressionInfo{i_id, i_binding_data->second, c_binding_data->second};
    // Note that even if I is the one that remains, C is the one that will be allocated, not I.
    FruitAssert(!i_binding_data->second.needsAllocation());
    i_binding_data->second = binding_data;
    binding_data_map.erase(c_binding_data);
#ifdef FRUIT_EXTRA_DEBUG
    std::cout << "InjectorStorage: performing binding compression for the edge " << i_id << "->" << c_id << std::endl;
#endif
  }

  // Copy the normalized bindings into the result vector.
  std::vector<std::pair<TypeId, BindingData>> result;
  result.reserve(binding_data_map.size());
  for (auto& p : binding_data_map) {
    result.push_back(p);
  }
  return result;
}

void BindingNormalization::addMultibindings(std::unordered_map<TypeId, NormalizedMultibindingData>& multibindings,
                                            FixedSizeAllocator::FixedSizeAllocatorData& fixed_size_allocator_data,
                                            std::vector<std::pair<TypeId, MultibindingData>>&& multibindingsVector) {

  std::sort(multibindingsVector.begin(), multibindingsVector.end(),
            typeInfoLessThanForMultibindings);
  
#ifdef FRUIT_EXTRA_DEBUG
  std::cout << "InjectorStorage: adding multibindings:" << std::endl;
#endif
  // Now we must merge multiple bindings for the same type.
  for (auto i = multibindingsVector.begin(); i != multibindingsVector.end(); /* no increment */) {
    const std::pair<TypeId, MultibindingData>& x = *i;
    NormalizedMultibindingData& b = multibindings[x.first];
    
    // Might be set already, but we need to set it if there was no multibinding for this type.
    b.get_multibindings_vector = x.second.get_multibindings_vector;
    
#ifdef FRUIT_EXTRA_DEBUG
    std::cout << x.first << " has " << std::distance(i, multibindingsVector.end()) << " multibindings." << std::endl;
#endif
    // Insert all multibindings for this type (note that x is also inserted here).
    for (; i != multibindingsVector.end() && i->first == x.first; ++i) {
      b.elems.push_back(NormalizedMultibindingData::Elem(i->second));
      if (i->second.needs_allocation) {
        fixed_size_allocator_data.addType(x.first);
      } else {
        fixed_size_allocator_data.addExternallyAllocatedType(x.first);
      }
    }
#ifdef FRUIT_EXTRA_DEBUG
    std::cout << std::endl;
#endif
  }
}

void BindingNormalization::expandLazyComponents(ComponentStorage& component, TypeId toplevel_component_fun_type_id) {
  // This set contains the lazy components whose expansion has already completed.
  HashSet<std::unique_ptr<LazyComponent>, HashComponentUniquePtr, ComponentUniquePtrEquals> fully_expanded_components =
      createHashSetWithCustomFunctors<std::unique_ptr<LazyComponent>>(HashComponentUniquePtr(), ComponentUniquePtrEquals());

  // If C1 is a toplevel lazy component that installs C2, that installs C3 and we're currently processing C3's bindings,
  // then this vector will be {C1, C2, C3}.
  std::vector<std::unique_ptr<LazyComponent>> components_expansion_stack;
  components_expansion_stack.reserve(10);

  // A set with the same elements as components_expansion_stack.
  // We use raw pointers here to avoid copying the LazyComponent objects.
  HashSet<LazyComponent*, HashComponentPtr, ComponentPtrEquals> components_with_expansion_in_progress =
      createHashSetWithCustomFunctors<LazyComponent*>(HashComponentPtr(), ComponentPtrEquals());

  // component.lazy_components contains the components that still need to be expanded (before duplicate detection, so
  // we may end up not expanding them when we get to them).
  // We use empty unique_ptr objects to mark the point where the expansion of a component finishes.

  while (!component.lazy_components.empty()) {
    FruitAssert(components_expansion_stack.size() == components_with_expansion_in_progress.size());

    std::unique_ptr<LazyComponent> lazy_component = std::move(component.lazy_components.back());

    if (!lazy_component) {
      component.lazy_components.pop_back();
      // A lazy component expansion has completed; we now move the component from
      // components_expansion_stack/components_with_expansion_in_progress to fully_expanded_components.
      lazy_component = std::move(components_expansion_stack.back());
      components_expansion_stack.pop_back();
      components_with_expansion_in_progress.erase(lazy_component.get());
      fully_expanded_components.insert(std::move(lazy_component));

      continue;
    }

    if (fully_expanded_components.count(lazy_component)) {
      // This lazy component was already inserted, skip it.
      component.lazy_components.pop_back();
      continue;
    }

    bool actually_inserted = components_with_expansion_in_progress.insert(lazy_component.get()).second;
    if (!actually_inserted) {
      printLazyComponentInstallationLoop(toplevel_component_fun_type_id, components_expansion_stack, lazy_component);
      exit(1);
    }

#ifdef FRUIT_EXTRA_DEBUG
    std::cout << "Expanding lazy component: " << lazy_component->getFunTypeId() << std::endl;
#endif

    // We put an empty unique_ptr as a marker. When we pop this marker, lazy_component's expansion will be complete.
    component.lazy_components.back() = std::unique_ptr<LazyComponent>();
    components_expansion_stack.push_back(std::move(lazy_component));

    // Note that this can also add other lazy components, so the resulting bindings can have a non-intuitive (although
    // deterministic) order.
    components_expansion_stack.back()->addBindings(component);
  }
}

} // namespace impl
} // namespace fruit
