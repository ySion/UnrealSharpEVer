#include "CSGenerator.h"
#include "CSModule.h"
#include "CSharpGeneratorUtilities.h"
#include "GlueGeneratorModule.h"
#include "CSScriptBuilder.h"
#include "PropertyTranslators/PropertyTranslator.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/SpringArmComponent.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopedSlowTask.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"

FName FCSGenerator::AllowableBlueprintVariableType = "BlueprintType";
FName FCSGenerator::NotAllowableBlueprintVariableType = "NotBlueprintType";
FName FCSGenerator::BlueprintSpawnableComponent = "BlueprintSpawnableComponent";
FName FCSGenerator::Blueprintable = "Blueprintable";
FName FCSGenerator::BlueprintFunctionLibrary = "BlueprintFunctionLibrary";

static const FName MD_WorldContext(TEXT("WorldContext"));
static const FName MD_WorldContextObject(TEXT("WorldContextObject"));


void FCSGenerator::StartGenerator(const FString& OutputDirectory)
{
	if (Initialized)
	{
		return;
	}

	Initialized = true;
	GeneratedScriptsDirectory = OutputDirectory;

	//TODO: SUPPORT THESE BUT CURRENTLY TOO LAZY TO FIX
	{
		Blacklist.AddClass("AnimationBlueprintLibrary");
		Blacklist.AddStruct(FSolverIterations::StaticStruct()->GetFName());
		Blacklist.AddFunctionCategory(UKismetMathLibrary::StaticClass()->GetFName(), "Math|Vector4");

		Whitelist.AddClass(USpringArmComponent::StaticClass()->GetFName());
		Whitelist.AddClass(UFloatingPawnMovement::StaticClass()->GetFName());
	}
	
	BlueprintInternalWhitelist.AddFunction(AActor::StaticClass()->GetFName(), GET_FUNCTION_NAME_CHECKED(AActor, UserConstructionScript));
		
	PropertyTranslators.Reset(new FCSSupportedPropertyTranslators(NameMapper, Blacklist));

	FModuleManager::Get().OnModulesChanged().AddRaw(this, &FCSGenerator::OnModulesChanged);

	// Generate base classes that are not generated by the above
	GenerateGlueForType(UInterface::StaticClass(), true);
	GenerateGlueForType(UObject::StaticClass(), true);

	TArray<UObject*> ObjectsToProcess;
	GetObjectsOfClass(UField::StaticClass(), ObjectsToProcess);
	GenerateGlueForTypes(ObjectsToProcess);
}

void FCSGenerator::OnModulesChanged(FName InModuleName, EModuleChangeReason InModuleChangeReason)
{
	if (InModuleChangeReason != EModuleChangeReason::ModuleLoaded)
	{
		return;
	}
	
	const UPackage* ModulePackage = FindPackage(nullptr, *(FString("/Script/") + InModuleName.ToString()));
	
	if (!ModulePackage)
	{
		return;
	}
	
	TArray<UObject*> ObjectsToProcess;
	GetObjectsWithPackage(ModulePackage, ObjectsToProcess);
	
	GenerateGlueForTypes(ObjectsToProcess);
}

#define LOCTEXT_NAMESPACE "FScriptGenerator"

void FCSGenerator::GenerateGlueForTypes(TArray<UObject*>& ObjectsToProcess)
{
	FScopedSlowTask SlowTask(1, LOCTEXT("GeneratingGlue", "Processing C# bindings..."));
	
	for (UObject* ObjectToProcess : ObjectsToProcess)
	{
		GenerateGlueForType(ObjectToProcess);
	}
	
	GeneratedFileManager.RenameTempFiles();
	SlowTask.EnterProgressFrame(1);
}

void FCSGenerator::GenerateGlueForType(UObject* Object, bool bForceExport)
{
	if (ExportedTypes.Contains(Object))
	{
		return;
	}

	FCSScriptBuilder Builder(FCSScriptBuilder::IndentType::Spaces);

	// We don't want stuff in the transient package - that stuff is just temporary
	if (Object->GetOutermost() == GetTransientPackage())
	{
		return;
	}
	
	if (UClass* Class = Cast<UClass>(Object))
	{
		if (Class->HasAnyFlags(RF_ClassDefaultObject))
		{
			return;
		}

		// If it's a SKEL class, we don't want to export it - those are just temporary classes used to hold the skeleton definition
		// of the blueprint class before it's compiled.
		if (Class->HasAnyFlags(RF_Transient) && Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
		{
			return;
		}

		// Don't generated glue for classes that have been regenerated in memory (this is the old version of the class)
		if (Class->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			return;
		}

		// Don't generate glue for TRASH_ classes. These are classes that have been deleted but are still in memory.
		if (Class->GetName().Find(TEXT("TRASH_")) != INDEX_NONE)
		{
			return;
		}

		// Don't generate glue for REINST_ classes. These are classes that have been recompiled but are still in memory, and will soon be TRASH_ classes.
		if (Class->GetName().Find(TEXT("REINST_")) != INDEX_NONE)
		{
			return;
		}
		
		RegisterClassToModule(Class);
				
		if (Class->IsChildOf(UInterface::StaticClass()))
		{
			ExportInterface(Class, Builder);
		}
		else if (bForceExport || ShouldExportClass(Class))
		{
			ExportClass(Class, Builder);
		}
	}
	else if (UScriptStruct* Struct = Cast<UScriptStruct>(Object))
	{
		if (bForceExport || ShouldExportStruct(Struct))
		{
			ExportStruct(Struct, Builder);
		}
	}
	else if (UEnum* Enum = Cast<UEnum>(Object))
	{
		if (bForceExport || ShouldExportEnum(Enum))
		{
			ExportEnum(Enum, Builder);
		}
	}

	if (Builder.IsEmpty())
	{
		return;
	}
	
	SaveTypeGlue(Object, Builder);
}

#undef LOCTEXT_NAMESPACE

bool FCSGenerator::CanExportClass(UClass* Class) const
{
	return !Blacklist.HasClass(Class);
}

bool FCSGenerator::CanDeriveFromNativeClass(UClass* Class)
{
	const bool bCanCreate = !Class->HasAnyClassFlags(CLASS_Deprecated) && !Class->HasAnyClassFlags(CLASS_NewerVersionExists) && !Class->ClassGeneratedBy;
	
	const bool bIsBlueprintBase = FKismetEditorUtilities::CanCreateBlueprintOfClass(Class);
	
	const bool bIsValidClass = bIsBlueprintBase || Whitelist.HasClass(Class) || Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass());

	return Class->IsChildOf(USubsystem::StaticClass()) || (bCanCreate && bIsValidClass);
}

static FString GetModuleExportFilename(FName ModuleFName)
{
	return ModuleFName.ToString() + TEXT("Module.cs");
}

void FCSGenerator::SaveModuleGlue(UPackage* Package, const FString& GeneratedGlue)
{
	const FCSModule& BindingsPtr = FindOrRegisterModule(Package);

	FString Filename = GetModuleExportFilename(BindingsPtr.GetModuleName());
	SaveGlue(&BindingsPtr, Filename, GeneratedGlue);
}

FString FCSGenerator::GetCSharpEnumType(const EPropertyType PropertyType) const
{
	switch (PropertyType)
	{
	case CPT_Int8:   return "sbyte";
	case CPT_Int16:  return "short";
	case CPT_Int:    return "int";
	case CPT_Int64:  return "long";
	case CPT_Byte:   return "byte";
	case CPT_UInt16: return "ushort";
	case CPT_UInt32: return "uint";
	case CPT_UInt64: return "ulong";
	default:
		return "";
	}
}

void FCSGenerator::ExportEnum(UEnum* Enum, FCSScriptBuilder& Builder)
{
	if (Whitelist.HasEnum(Enum) || !Blacklist.HasEnum(Enum))
	{
		const FCSModule& Module = FindOrRegisterModule(Enum);

		Builder.GenerateScriptSkeleton(Module.GetNamespace());
		Builder.AppendLine(TEXT("[UEnum]"));
		Builder.DeclareType("enum", *Enum->GetName(), "byte", false, false);
		
		FString CommonPrefix;
		int32 SkippedValueCount = 0;
		
		TArray<FString> EnumValues;

		const int32 ValueCount = Enum->NumEnums();
		EnumValues.Reserve(ValueCount);
		
		for (int32 i = 0; i < ValueCount; ++i)
		{
			FString& RawName = *new(EnumValues) FString();

			if (!ShouldExportEnumEntry(Enum, i))
			{
				RawName = FString();
				SkippedValueCount++;
				continue;
			}

			FName ValueName = Enum->GetNameByIndex(i);
			FString QualifiedValueName = ValueName.ToString();
			const int32 ColonPosition = QualifiedValueName.Find("::");

			if (ColonPosition != INDEX_NONE)
			{
				RawName = QualifiedValueName.Mid(ColonPosition + 2);
			}
			else
			{
				RawName = QualifiedValueName;
			}

			if (i == ValueCount - 1 && RawName.EndsWith("MAX"))
			{
				++SkippedValueCount;
				EnumValues.Pop(false);
				continue;
			}
		}

		for (int32 i = 0; i < EnumValues.Num(); ++i)
		{
			FString& EnumValue = EnumValues[i];
			
			if (EnumValue.IsEmpty())
			{
				continue;
			}
			
			Builder.AppendLine(FString::Printf(TEXT("%s=%d,"), *EnumValues[i], i));
		}

		Builder.CloseBrace();
	}
}

bool FCSGenerator::CanExportFunction(const UStruct* Struct, const UFunction* Function) const
{
	if (Blacklist.HasFunction(Struct, Function) && !Whitelist.HasFunction(Struct, Function) || !ShouldExportFunction(Function))
	{
		return false;
	}

	if (Function->HasMetaData(MD_Latent) || Function->HasMetaData(MD_BlueprintInternalUseOnly))
	{
		return BlueprintInternalWhitelist.HasFunction(Struct, Function);
	}

	return CanExportFunctionParameters(Function);
}

bool FCSGenerator::CanExportFunctionParameters(const UFunction* Function) const
{
	const FProperty* ReturnProperty = Function->GetReturnProperty();
	
	if (ReturnProperty != nullptr && !CanExportReturnValue(ReturnProperty))
	{
		return false;
	}

	for (TFieldIterator<FProperty> ParamIt(Function); ParamIt && !ParamIt->HasAnyPropertyFlags(CPF_ReturnParm); ++ParamIt)
	{
		if (!CanExportParameter(*ParamIt))
		{
			return false;
		}
	}

	return true;
}

bool FCSGenerator::CanExportParameter(const FProperty* Property) const
{
	bool bCanExport = Property->ArrayDim == 1;

	if (bCanExport)
	{
		const FPropertyTranslator& Handler = PropertyTranslators->Find(Property);
		if (!Handler.IsSupportedAsParameter() || !Handler.CanHandleProperty(Property))
		{
			bCanExport = false;
		}
	}

	if (!bCanExport)
	{
		++UnhandledParameters.FindOrAdd(Property->GetClass()->GetFName());
	}

	return bCanExport;
}

bool FCSGenerator::CanExportProperty(const UStruct* Struct, const FProperty* Property) const
{
	// Always include UProperties for whitelisted structs.
	// If their properties where blueprint-exposed, we wouldn't have had to whitelist them!
	bool bCanExport = !Blacklist.HasProperty(Struct, Property)
	&& (CanExportPropertyShared(Property) || Whitelist.HasProperty(Struct, Property) || Whitelist.HasStruct(Struct));
	
	if (bCanExport)
	{
		const bool bIsClassProperty = Struct->IsA(UClass::StaticClass());
		check(bIsClassProperty || Struct->IsA(UScriptStruct::StaticClass()));

		const FPropertyTranslator& Handler = PropertyTranslators->Find(Property);
		if ((bIsClassProperty && !Handler.IsSupportedAsProperty()) || (!bIsClassProperty && !Handler.IsSupportedAsStructProperty())  || !Handler.CanHandleProperty(Property))
		{
			++UnhandledProperties.FindOrAdd(Property->GetClass()->GetFName());
			bCanExport = false;
		}
	}

	return bCanExport;
}

bool FCSGenerator::CanExportPropertyShared(const FProperty* Property) const
{
	const FPropertyTranslator& Handler = PropertyTranslators->Find(Property);

	// must be blueprint visible, should not be deprecated, arraydim == 1
	//if it's CPF_BlueprintVisible, we know it's RF_Public, CPF_Protected or MD_AllowPrivateAccess
	const bool bCanExport = ShouldExportProperty(Property)
		&& !Property->HasAnyPropertyFlags(CPF_Deprecated)
		&& (Property->ArrayDim == 1 || (Handler.IsSupportedInStaticArray() && Property->GetOutermost()->IsA(UClass::StaticClass())));

	return bCanExport;
}

void FCSGenerator::GetExportedProperties(TSet<FProperty*>& ExportedProperties, const UStruct* Struct)
{
	for (TFieldIterator<FProperty> PropertyIt(Struct, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		
		if (!CanExportProperty(Struct, Property))
		{
			continue;
		}

		ExportedProperties.Add(Property);
	}
}

void FCSGenerator::GetExportedFunctions(TSet<UFunction*>& ExportedFunctions, TSet<UFunction*>& ExportedOverridableFunctions, const UClass* Class)
{
	for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
	{
		UFunction* Function = *FunctionIt;
		
		if (!CanExportFunction(Class, Function))
		{
			continue;
		}
		
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			ExportedOverridableFunctions.Add(Function);
		}
		else
		{
			ExportedFunctions.Add(Function);
		}
	}
	
	for (const FImplementedInterface& Interface : Class->Interfaces)
	{
		for (TFieldIterator<UFunction> It(Interface.Class); It; ++It)
		{
			UFunction* Function = *It;
			
			if (!CanExportFunction(Class, Function) || !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				continue;
			}

			bool bIsOverriden = false;
			for (UFunction* ExportedOverridableFunction : ExportedOverridableFunctions)
			{
				if (Function->GetFName() == ExportedOverridableFunction->GetFName())
				{
					bIsOverriden = true;
					break;
				}
			}

			if (bIsOverriden)
			{
				continue;
			}
			
			ExportedOverridableFunctions.Add(Function);
		}
	}
}

void FCSGenerator::GetExportedStructs(TSet<UScriptStruct*>& ExportedStructs) const
{
	for(TObjectIterator<UScriptStruct> ScriptStructItr; ScriptStructItr; ++ScriptStructItr)
	{
		UScriptStruct* Struct = *ScriptStructItr;
		
		if (Blacklist.HasStruct(Struct))
		{
			continue;
		}

		ExportedStructs.Add(Struct);
	}
}

bool FCSGenerator::CanExportOverridableParameter(const FProperty* Property)
{
	bool bCanExport = Property->ArrayDim == 1;

	if (bCanExport)
	{
		const FPropertyTranslator& Handler = PropertyTranslators->Find(Property);
		bCanExport = Handler.IsSupportedAsOverridableFunctionParameter() && Handler.CanHandleProperty(Property);
	}

	if (!bCanExport)
	{
		++UnhandledOverridableParameters.FindOrAdd(Property->GetClass()->GetFName());
	}

	return bCanExport;
}

bool FCSGenerator::CanExportOverridableReturnValue(const FProperty* Property)
{
	bool bCanExport = Property->ArrayDim == 1;

	if (bCanExport)
	{
		const FPropertyTranslator& Handler = PropertyTranslators->Find(Property);
		if (!Handler.IsSupportedAsOverridableFunctionReturnValue() || !Handler.CanHandleProperty(Property))
		{
			bCanExport = false;
		}
	}

	if (!bCanExport)
	{
		++UnhandledOverridableReturnValues.FindOrAdd(Property->GetClass()->GetFName());
	}

	return bCanExport;
}

const FString& FCSGenerator::GetNamespace(const UObject* Object)
{
	const FCSModule& Module = FindOrRegisterModule(Object);
	return Module.GetNamespace();
}

void FCSGenerator::RegisterClassToModule(const UObject* Struct)
{
	FindOrRegisterModule(Struct);
}

FCSModule& FCSGenerator::FindOrRegisterModule(const UObject* Struct)
{
	const FName ModuleName = GetModuleFName(Struct);

	FCSModule* BindingsModule = CSharpBindingsModules.Find(ModuleName);
    
	if (!BindingsModule)
	{
		FString Directory = TEXT("");
		FString ProjectDirectory = FPaths::ProjectDir();
		FString GeneratedUserContent = "Script/obj/Generated";

		if (TSharedPtr<IPlugin> ThisPlugin = IPluginManager::Get().FindPlugin(TEXT("UnrealSharp")))
		{
			// If this plugin is a project plugin, we want to generate all the bindings in the same directory as the plug-in
			// since there's no reason to split the project from the plug-in, like you would need to if this was installed
			// as an engine plugin.
			if (ThisPlugin->GetType() == EPluginType::Project)
			{
				Directory = GeneratedScriptsDirectory;
			}
			else
			{
				if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().GetModuleOwnerPlugin(*ModuleName.ToString()))
				{
					if (Plugin->GetType() == EPluginType::Engine || Plugin->GetType() == EPluginType::Enterprise)
					{
						Directory = GeneratedScriptsDirectory;
					}
					else
					{
						Directory = FPaths::Combine(ProjectDirectory, GeneratedUserContent);
					}
				}
				else
				{
					if (IModuleInterface* Module = FModuleManager::Get().GetModule(ModuleName))
					{
						if (Module->IsGameModule())
						{
							Directory = FPaths::Combine(ProjectDirectory, GeneratedUserContent);
						}
						else
						{
							Directory = GeneratedScriptsDirectory;
						}
					}
					else
					{
						// This is awful, but we have no way of knowing if the module is a game module or not without loading it.
						// Also for whatever reason "CoreOnline" is not a module.
						Directory = GeneratedScriptsDirectory;
					}
				}
			}
		}

		ensureMsgf(!Directory.IsEmpty(), TEXT("Generating the directory location for generating the scripts for this module failed."));

		BindingsModule = &CSharpBindingsModules.Emplace(ModuleName, FCSModule(ModuleName, Directory));
	}

	return *BindingsModule;
}

void FCSGenerator::ExportInterface(UClass* Interface, FCSScriptBuilder& Builder)
{
	FString InterfaceName = NameMapper.GetScriptClassName(Interface);
	const FCSModule& BindingsModule = FindOrRegisterModule(Interface);
	
	Builder.GenerateScriptSkeleton(BindingsModule.GetNamespace());
	Builder.DeclareType("interface", InterfaceName, "", false);

	TSet<UFunction*> ExportedFunctions;
	TSet<UFunction*> ExportedOverridableFunctions;
	GetExportedFunctions(ExportedFunctions, ExportedOverridableFunctions, Interface);
	
	ExportInterfaceFunctions(Builder, Interface, ExportedOverridableFunctions);

	Builder.CloseBrace();
}

void FCSGenerator::ExportClass(UClass* Class, FCSScriptBuilder& Builder)
{
	if (!ensure(!ExportedTypes.Contains(Class)))
	{
		return;
	}

	ExportedTypes.Add(Class);

	Builder.AppendLine(TEXT("// This file is automatically generated"));
	
	if (UClass* SuperClass = Class->GetSuperClass())
	{
		GenerateGlueForType(SuperClass, true);
	}
	
	const FString ScriptClassName = NameMapper.GetScriptClassName(Class);
	const FCSModule& BindingsModule = FindOrRegisterModule(Class);
	
	TSet<FProperty*> ExportedProperties;
	TSet<UFunction*> ExportedFunctions;
	TSet<UFunction*> ExportedOverridableFunctions;

	GetExportedProperties(ExportedProperties, Class);
	GetExportedFunctions(ExportedFunctions, ExportedOverridableFunctions, Class);

	TArray<FString> Interfaces;
	{
		TSet<FString> DeclaredDirectives;
		
		for (const FImplementedInterface& ImplementedInterface : Class->Interfaces)
		{
			UClass* InterfaceClass = ImplementedInterface.Class;

			if (FKismetEditorUtilities::IsClassABlueprintImplementableInterface(InterfaceClass))
			{
				Interfaces.Add(InterfaceClass->GetName());
				
				const FCSModule& InterfaceModule = FindOrRegisterModule(InterfaceClass);

				const FString& InterfaceNamespace = InterfaceModule.GetNamespace();
				
				if (DeclaredDirectives.Contains(InterfaceNamespace))
				{
					continue;
				}
					
				Builder.DeclareDirective(InterfaceNamespace);
				DeclaredDirectives.Add(InterfaceNamespace);
			}
		}
	}

	Builder.GenerateScriptSkeleton(BindingsModule.GetNamespace());
	Builder.AppendLine(TEXT("[UClass]"));
	Builder.DeclareType("class", ScriptClassName, GetSuperClassName(Class), Class->HasAnyClassFlags(CLASS_Abstract), true, Interfaces);

	// Generate static constructor
	Builder.AppendLine();
	ExportStaticConstructor(Builder, Class, ExportedProperties, ExportedFunctions, ExportedOverridableFunctions);

	//generate inheriting constructor
	Builder.AppendLine();
	Builder.AppendLine(FString::Printf(TEXT("protected %s(IntPtr nativeObject) : base(nativeObject)"), *ScriptClassName));
	Builder.OpenBrace();
	Builder.CloseBrace();

	ExportClassProperties(Builder, Class, ExportedProperties);
	ExportClassFunctions(Builder, Class, ExportedFunctions);
	ExportClassOverridableFunctions(Builder, ExportedOverridableFunctions);	
	
	Builder.AppendLine();
	Builder.CloseBrace();
}

void FCSGenerator::ExportClassOverridableFunctions(FCSScriptBuilder& Builder, const TSet<UFunction*>& ExportedOverridableFunctions)
{
	for (UFunction* Function : ExportedOverridableFunctions)
	{
		PropertyTranslators->Find(Function).ExportOverridableFunction(Builder, Function);
	}
}

void FCSGenerator::ExportClassFunctions(FCSScriptBuilder& Builder, const UClass* Class, const TSet<UFunction*>& ExportedFunctions)
{
	for (UFunction* Function : ExportedFunctions)
	{
		FPropertyTranslator::FunctionType FuncType = FPropertyTranslator::FunctionType::Normal;
		
		if (Function->HasAnyFunctionFlags(FUNC_Static) && IsBlueprintFunctionLibrary(Class))
		{
			ExtensionMethod Method;
			if (GetExtensionMethodInfo(Method, *Function))
			{
				FuncType = FPropertyTranslator::FunctionType::ExtensionOnAnotherClass;

				const FCSModule& BindingsModule = FindOrRegisterModule(Class);
				TArray<ExtensionMethod>& ModuleExtensionMethods = ExtensionMethods.FindOrAdd(BindingsModule.GetModuleName());
				ModuleExtensionMethods.Add(Method);
			}
		}

		PropertyTranslators->Find(Function).ExportFunction(Builder, Function, FuncType);
	}
}

void FCSGenerator::ExportInterfaceFunctions(FCSScriptBuilder& Builder, const UClass* Class, const TSet<UFunction*>& ExportedFunctions) const
{
	for (UFunction* Function : ExportedFunctions)
	{
		PropertyTranslators->Find(Function).ExportInterfaceFunction(Builder, Function);
	}
}

void FCSGenerator::ExportClassProperties(FCSScriptBuilder& Builder, const UClass* Class, TSet<FProperty*>& ExportedProperties)
{
	for (FProperty* Property : ExportedProperties)
	{
		const FPropertyTranslator& PropertyTranslator = PropertyTranslators->Find(Property);
		PropertyTranslator.ExportWrapperProperty(Builder, Property, Greylist.HasProperty(Class, Property), Whitelist.HasProperty(Class, Property));
	}
}

void FCSGenerator::ExportStaticConstructor(FCSScriptBuilder& Builder, const UStruct* Struct ,const TSet<FProperty*>& ExportedProperties,  const TSet<UFunction*>& ExportedFunctions, const TSet<UFunction*>& ExportedOverrideableFunctions)
{
	const UClass* Class = Cast<UClass>(Struct);
	const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Struct);

	if (!ScriptStruct)
	{
		if (ExportedProperties.IsEmpty() && ExportedFunctions.IsEmpty() && ExportedOverrideableFunctions.IsEmpty())
		{
			return;
		}
	}
	
	bool bHasStaticFunctions = false;
	
	for (UFunction* ExportedFunction : ExportedFunctions)
	{
		if (ExportedFunction->HasAnyFunctionFlags(FUNC_Static))
		{
			bHasStaticFunctions = true;
			break;
		}
	}

	if (bHasStaticFunctions)
	{
		// Keep the class pointer so we can use the CDO to invoke static functions.
		Builder.AppendLine("static readonly IntPtr NativeClassPtr;");
	}

	if (ScriptStruct)
	{
		Builder.AppendLine("public static readonly int NativeDataSize;");
	}

	FString TypeName = NameMapper.GetTypeScriptName(Struct);
	
	Builder.AppendLine(FString::Printf(TEXT("static %s()"), *TypeName));
	Builder.OpenBrace();

	Builder.AppendLine(FString::Printf(TEXT("%sNativeClassPtr = %s.CallGetNative%sFromName(\"%s\");"),
		bHasStaticFunctions ? TEXT("") : TEXT("IntPtr "),
		CoreUObjectCallbacks,
		Class ? TEXT("Class") : TEXT("Struct"), 
		*Struct->GetName()));

	Builder.AppendLine();

	ExportPropertiesStaticConstruction(Builder, ExportedProperties);

	if (Class)
	{
		Builder.AppendLine();
		ExportClassFunctionsStaticConstruction(Builder, ExportedFunctions);

		Builder.AppendLine();
		ExportClassOverridableFunctionsStaticConstruction(Builder, ExportedOverrideableFunctions);

		Builder.AppendLine();
	}
	else
	{
		Builder.AppendLine();
		Builder.AppendLine(FString::Printf(TEXT("NativeDataSize = %s.CallGetNativeStructSize(NativeClassPtr);"), UScriptStructCallbacks));
	}

	Builder.CloseBrace();
}

void FCSGenerator::ExportClassOverridableFunctionsStaticConstruction(FCSScriptBuilder& Builder, const TSet<UFunction*>& ExportedOverridableFunctions) const
{
	for (UFunction* Function : ExportedOverridableFunctions)
	{
		if (Function->NumParms == 0)
		{
			continue;
		}

		FString NativeMethodName = Function->GetName();
		Builder.AppendLine(FString::Printf(TEXT("IntPtr %s_NativeFunction = %s.CallGetNativeFunctionFromClassAndName(NativeClassPtr, \"%s\");"), *NativeMethodName, UClassCallbacks, *NativeMethodName));
		Builder.AppendLine(FString::Printf(TEXT("%s_ParamsSize = %s.CallGetNativeFunctionParamsSize(%s_NativeFunction);"), *NativeMethodName, UFunctionCallbacks, *NativeMethodName));
		for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FPropertyTranslator& ParamHandler = PropertyTranslators->Find(Property);
			ParamHandler.ExportParameterStaticConstruction(Builder, NativeMethodName, Property);
		}

		Builder.AppendLine();
	}
}

void FCSGenerator::ExportClassFunctionsStaticConstruction(FCSScriptBuilder& Builder, const TSet<UFunction*>& ExportedFunctions)
{
	for (const UFunction* Function : ExportedFunctions)
	{
		ExportClassFunctionStaticConstruction(Builder, Function);
	}
}

void FCSGenerator::ExportClassFunctionStaticConstruction(FCSScriptBuilder& Builder, const UFunction *Function)
{
	FString NativeMethodName = Function->GetName();
	Builder.AppendLine(FString::Printf(TEXT("%s_NativeFunction = %s.CallGetNativeFunctionFromClassAndName(NativeClassPtr, \"%s\");"), *NativeMethodName, UClassCallbacks, *Function->GetName()));
	
	if (Function->NumParms > 0)
	{
		Builder.AppendLine(FString::Printf(TEXT("%s_ParamsSize = %s.CallGetNativeFunctionParamsSize(%s_NativeFunction);"), *NativeMethodName, UFunctionCallbacks, *NativeMethodName));
	}
	
	for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FProperty* Property = *It;
		const FPropertyTranslator& ParamHandler = PropertyTranslators->Find(Property);
		ParamHandler.ExportParameterStaticConstruction(Builder, NativeMethodName, Property);
	}
}

void FCSGenerator::ExportPropertiesStaticConstruction(FCSScriptBuilder& Builder, const TSet<FProperty*>& ExportedProperties)
{
	//we already warn on conflicts when exporting the properties themselves, so here we can just silently skip them
	TSet<FString> ExportedPropertiesHash;

	for (FProperty* Property : ExportedProperties)
	{
		FString ManagedName = NameMapper.MapPropertyName(Property);
		
		if (ExportedPropertiesHash.Contains(ManagedName))
		{
			continue;
		}
		
		ExportedPropertiesHash.Add(ManagedName);
		PropertyTranslators->Find(Property).ExportPropertyStaticConstruction(Builder, Property, Property->GetName());
	}
}

bool FCSGenerator::GetExtensionMethodInfo(ExtensionMethod& Info, UFunction& Function)
{
	FProperty* SelfParameter = nullptr;
	bool IsWorldContextParameter = false;

	// ScriptMethod is the canonical metadata for extension methods
	if (Function.HasMetaData(ScriptMethodMetaDataKey))
	{
		SelfParameter = CastField<FProperty>(Function.ChildProperties);
	}

	// however, we can also convert DefaultToSelf parameters to extension methods
	if (!SelfParameter && Function.HasMetaData(MD_DefaultToSelf))
	{
		SelfParameter = Function.FindPropertyByName(*Function.GetMetaData(MD_DefaultToSelf));
	}

	// if a world context is specified, we can use that to determine whether the parameter is a world context
	// we can also convert WorldContext methods into extension methods, if we didn't match on some other parameter already
	if (Function.HasMetaData(MD_WorldContext))
	{
		FString WorldContextName = Function.GetMetaData(MD_WorldContext);
		if (SelfParameter)
		{
			if (SelfParameter->GetName() == WorldContextName)
			{
				IsWorldContextParameter = true;
			}
		}
		else
		{
			SelfParameter = Function.FindPropertyByName(*WorldContextName);
			IsWorldContextParameter = true;
		}
	}

	if (!SelfParameter)
	{
		return false;
	}

	// some world context parameters might not be annotated, so check the name
	if (!IsWorldContextParameter)
	{
		FName ParamName = SelfParameter->GetFName();
		IsWorldContextParameter |= ParamName == MD_WorldContext || ParamName == MD_WorldContextObject;
	}

	Info.Function = &Function;
	Info.SelfParameter = SelfParameter;
	Info.OverrideClassBeingExtended = nullptr;

	// if it's a world context, type it more strongly
	if (IsWorldContextParameter)
	{
		Info.OverrideClassBeingExtended = UWorld::StaticClass();
	}

	return true;
}

void FCSGenerator::ExportStruct(UScriptStruct* Struct, FCSScriptBuilder& Builder)
{
	const FCSModule& BindingsModule = FindOrRegisterModule(Struct);

	TSet<FProperty*> ExportedProperties;
	GetExportedProperties(ExportedProperties, Struct);
	
	Builder.GenerateScriptSkeleton(BindingsModule.GetNamespace());

	const bool bIsBlittable = PropertyTranslators->IsStructBlittable(*Struct);
	
	FCSPropertyBuilder PropBuilder;

	PropBuilder.AddAttribute("UStruct");
		
	if (bIsBlittable)
	{
		PropBuilder.AddArgument("IsBlittable = true");
	}
	
	PropBuilder.Finish();

	Builder.AppendLine(PropBuilder.ToString());

	Builder.DeclareType("struct", NameMapper.GetStructScriptName(Struct));
	
	ExportStructProperties(Builder, Struct, ExportedProperties, bIsBlittable);

	if (!bIsBlittable)
	{
		// Generate static constructor
		Builder.AppendLine();
		
		ExportStaticConstructor(Builder, Struct, ExportedProperties, {}, {});

		// Generate native constructor
		Builder.AppendLine();
		ExportMirrorStructMarshalling(Builder, Struct, ExportedProperties);
	}
	
	Builder.CloseBrace();

	if (!bIsBlittable)
	{
		// Generate custom marshaler for arrays of this struct
		ExportStructMarshaller(Builder, Struct);
	}
}


void FCSGenerator::ExportStructProperties(FCSScriptBuilder& Builder, const UStruct* Struct, const TSet<FProperty*>& ExportedProperties, bool bSuppressOffsets) const
{
	for (FProperty* Property : ExportedProperties)
	{
		const FPropertyTranslator& PropertyTranslator = PropertyTranslators->Find(Property);
		PropertyTranslator.ExportMirrorProperty(Builder, Property, Greylist.HasProperty(Struct, Property), bSuppressOffsets);
	}
}

void FCSGenerator::ExportStructMarshaller(FCSScriptBuilder& Builder, const UScriptStruct* Struct)
{
	FString StructName = NameMapper.GetStructScriptName(Struct);

	Builder.AppendLine();
	Builder.AppendLine(FString::Printf(TEXT("public static class %sMarshaler"), *StructName));
	Builder.OpenBrace();

	Builder.AppendLine(FString::Printf(TEXT("public static %s FromNative(IntPtr nativeBuffer, int arrayIndex, UnrealSharpObject owner)"), *StructName));
	Builder.OpenBrace();
	Builder.AppendLine(FString::Printf(TEXT("return new %s(nativeBuffer + arrayIndex * GetNativeDataSize());"), *StructName));
	Builder.CloseBrace(); // MarshalNativeToManaged

	Builder.AppendLine();
	Builder.AppendLine(FString::Printf(TEXT("public static void ToNative(IntPtr nativeBuffer, int arrayIndex, %s owner, %s obj)"), UNREAL_SHARP_OBJECT, *StructName));
	Builder.OpenBrace();
	Builder.AppendLine("obj.ToNative(nativeBuffer + arrayIndex * GetNativeDataSize());");
	Builder.CloseBrace(); // MarshalManagedToNative

	Builder.AppendLine();
	Builder.AppendLine("public static int GetNativeDataSize()");
	Builder.OpenBrace();
	Builder.AppendLine(FString::Printf(TEXT("return %s.NativeDataSize;"), *StructName));
	Builder.CloseBrace();
	Builder.CloseBrace();
}

void FCSGenerator::ExportMirrorStructMarshalling(FCSScriptBuilder& Builder, const UScriptStruct* Struct, TSet<FProperty*> ExportedProperties) const
{
	Builder.AppendLine();
	Builder.AppendLine("// Construct by marshalling from a native buffer.");
	Builder.AppendLine(FString::Printf(TEXT("public %s(IntPtr InNativeStruct)"), *NameMapper.GetStructScriptName(Struct)));
	Builder.OpenBrace();
	Builder.BeginUnsafeBlock();

	for (FProperty* Property : ExportedProperties)
	{
		const FPropertyTranslator& PropertyHandler = PropertyTranslators->Find(Property);
		FString NativePropertyName = Property->GetName();
		FString CSharpPropertyName = NameMapper.MapPropertyName(Property);
		PropertyHandler.ExportMarshalFromNativeBuffer(
			Builder, 
			Property, 
			"null",
			NativePropertyName,
			FString::Printf(TEXT("%s ="), *CSharpPropertyName),
			"InNativeStruct", 
			FString::Printf(TEXT("%s_Offset"),*NativePropertyName),
			false,
			false);
	}

	Builder.EndUnsafeBlock();
	Builder.CloseBrace(); // ctor
	
	Builder.AppendLine();
	Builder.AppendLine("// Marshal into a preallocated native buffer.");
	Builder.AppendLine("public void ToNative(IntPtr Buffer)");
	Builder.OpenBrace();
	Builder.BeginUnsafeBlock();

	for (const FProperty* Property : ExportedProperties)
	{
		const FPropertyTranslator& PropertyHandler = PropertyTranslators->Find(Property);
		FString NativePropertyName = Property->GetName();
		FString CSharpPropertyName = NameMapper.MapPropertyName(Property);
		PropertyHandler.ExportMarshalToNativeBuffer(
			Builder, 
			Property, 
			"null",
			NativePropertyName,
			"Buffer", 
			FString::Printf(TEXT("%s_Offset"), *NativePropertyName),
			CSharpPropertyName);
	}

	Builder.EndUnsafeBlock();
	Builder.CloseBrace(); // ToNative
}

FString FCSGenerator::GetSuperClassName(const UClass* Class) const
{
	if (Class == UObject::StaticClass())
	{
		return UNREAL_SHARP_OBJECT;
	}

	// For all other classes, return the fully qualified name of the superclass
	const UClass* SuperClass = Class->GetSuperClass();
	return NameMapper.GetQualifiedName(SuperClass);
}

void FCSGenerator::SaveTypeGlue(const UObject* Object, const FCSScriptBuilder& ScriptBuilder)
{
	const FCSModule& BindingsPtr = FindOrRegisterModule(Object);

	const FString FileName = FString::Printf(TEXT("%s.generated.cs"), *Object->GetName());
	SaveGlue(&BindingsPtr, FileName, ScriptBuilder.ToString());
}

void FCSGenerator::SaveGlue(const FCSModule* Bindings, const FString& Filename, const FString& GeneratedGlue)
{
	const FString& BindingsSourceDirectory = Bindings->GetGeneratedSourceDirectory();

	IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
	if (!File.CreateDirectoryTree(*BindingsSourceDirectory))
	{
		UE_LOG(LogGlueGenerator, Error, TEXT("Could not create directory %s"), *BindingsSourceDirectory);
		return;
	}

	const FString GlueOutputPath = FPaths::Combine(*BindingsSourceDirectory, *Filename);
	GeneratedFileManager.SaveFileIfChanged(GlueOutputPath, GeneratedGlue);
}

bool FCSGenerator::CanExportReturnValue(const FProperty* Property) const
{
	bool bCanExport = Property->ArrayDim == 1;

	if (bCanExport)
	{
		const FPropertyTranslator& Handler = PropertyTranslators->Find(Property);
		if (!Handler.IsSupportedAsReturnValue() || !Handler.CanHandleProperty(Property))
		{
			++UnhandledReturnValues.FindOrAdd(Property->GetClass()->GetFName());
			bCanExport = false;
		}
	}

	return bCanExport;
}

