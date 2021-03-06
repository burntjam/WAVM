#include "Inline/BasicTypes.h"
#include "Inline/Timing.h"
#include "Logging/Logging.h"
#include "Runtime.h"
#include "RuntimePrivate.h"
#include "Intrinsics.h"

#include <set>
#include <vector>

namespace Runtime
{
	// Keep a global list of all objects.
	struct GCGlobals
	{
		Platform::Mutex* mutex;
		std::set<ObjectImpl*> allObjects;

		static GCGlobals& get()
		{
			static GCGlobals globals;
			return globals;
		}
		
	private:
		GCGlobals(): mutex(Platform::createMutex()) {}
	};

	ObjectImpl::ObjectImpl(ObjectKind inKind)
	: Object(inKind), numRootReferences(0)
	{
		// Add the object to the global array.
		Platform::Lock lock(GCGlobals::get().mutex);
		GCGlobals::get().allObjects.insert(this);
	}

	void addGCRoot(Object* object)
	{
		ObjectImpl* gcObject = (ObjectImpl*)object;
		++gcObject->numRootReferences;
	}

	void removeGCRoot(Object* object)
	{
		ObjectImpl* gcObject = (ObjectImpl*)object;
		--gcObject->numRootReferences;
	}

	void collectGarbage()
	{
		GCGlobals& gcGlobals = GCGlobals::get();
		Platform::Lock lock(gcGlobals.mutex);
		Timing::Timer timer;

		std::set<Object*> referencedObjects;
		std::vector<Object*> pendingScanObjects;
		
		// Initialize the referencedObjects set from the rooted object set.
		Uptr numRoots = 0;
		for(auto object : gcGlobals.allObjects)
		{
			if(object && object->numRootReferences > 0)
			{
				referencedObjects.insert(object);
				pendingScanObjects.push_back(object);
				++numRoots;
			}
		}

		// Scan the objects added to the referenced set so far: gather their child references and recurse.
		while(pendingScanObjects.size())
		{
			Object* scanObject = pendingScanObjects.back();
			pendingScanObjects.pop_back();

			// Gather the child references for this object based on its kind.
			std::vector<Object*> childReferences;
			switch(scanObject->kind)
			{
			case ObjectKind::function:
			{
				FunctionInstance* function = asFunction(scanObject);
				childReferences.push_back(function->moduleInstance);
				break;
			}
			case ObjectKind::table:
			{
				TableInstance* table = asTable(scanObject);
				childReferences.push_back(table->compartment);
				childReferences.insert(childReferences.end(),table->elements.begin(),table->elements.end());
				break;
			}
			case ObjectKind::memory:
			{
				MemoryInstance* memory = asMemory(scanObject);
				childReferences.push_back(memory->compartment);
				break;
			}
			case ObjectKind::global:
			{
				GlobalInstance* global = asGlobal(scanObject);
				childReferences.push_back(global->compartment);
				break;
			}
			case ObjectKind::module:
			{
				ModuleInstance* moduleInstance = asModule(scanObject);
				childReferences.push_back(moduleInstance->compartment);
				childReferences.insert(childReferences.begin(),moduleInstance->functionDefs.begin(),moduleInstance->functionDefs.end());
				childReferences.insert(childReferences.begin(),moduleInstance->functions.begin(),moduleInstance->functions.end());
				childReferences.insert(childReferences.begin(),moduleInstance->tables.begin(),moduleInstance->tables.end());
				childReferences.insert(childReferences.begin(),moduleInstance->memories.begin(),moduleInstance->memories.end());
				childReferences.insert(childReferences.begin(),moduleInstance->globals.begin(),moduleInstance->globals.end());
				childReferences.push_back(moduleInstance->defaultMemory);
				childReferences.push_back(moduleInstance->defaultTable);
				break;
			}
			case ObjectKind::context:
			{
				Context* context = asContext(scanObject);
				childReferences.push_back(context->compartment);
				break;
			}
			case ObjectKind::compartment:
			{
				Compartment* compartment = asCompartment(scanObject);
				childReferences.push_back(compartment->wavmIntrinsics);
				break;
			}

			case ObjectKind::exceptionType:
				break;

			default: Errors::unreachable();
			};

			// Add the object's child references to the referenced set, and enqueue them for scanning.
			for(auto reference : childReferences)
			{
				if(reference && !referencedObjects.count(reference))
				{
					referencedObjects.insert(reference);
					pendingScanObjects.push_back(reference);
				}
			}
		};

		// Find the objects that weren't reached, and call finalize on each of them.
		std::vector<ObjectImpl*> finalizedObjects;
		auto objectIt = gcGlobals.allObjects.begin();
		while(objectIt != gcGlobals.allObjects.end())
		{
			if(referencedObjects.count(*objectIt)) { ++objectIt; }
			else
			{
				ObjectImpl* object = *objectIt;
				objectIt = gcGlobals.allObjects.erase(objectIt);
				object->finalize();
				finalizedObjects.push_back(object);
			}
		}

		// Delete all the finalized objects.
		for(ObjectImpl* object : finalizedObjects)
		{
			delete object;
		}

		Log::printf(Log::Category::metrics,"Collected garbage in %.2fms: %u roots, %u objects, %u garbage\n",
			timer.getMilliseconds(),
			numRoots,gcGlobals.allObjects.size() + finalizedObjects.size(),finalizedObjects.size());
	}
}