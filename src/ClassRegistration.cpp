#include <RE/Skyrim.h>

#include "ClassRegistration.h"

namespace
{
	constexpr auto kClassName = "hkbExtrapolatingTransitionEffect";
	constexpr auto kBaseName = "hkbBlendingTransitionEffect";
	constexpr std::size_t kSerializedSize = 0x170;
	RE::hkClass reflectedClass{};
	RE::hkTypeInfo typeInfo{};
	RE::hkTypeInfo::Finish baseFinish{};
	std::array<RE::hkClassMember, 9> ignoredMembers{};

	void Finish(void* object, int flag)
	{
		baseFinish(object, flag);
		std::memcpy(object, &typeInfo.vtable, sizeof(typeInfo.vtable));
	}
}

const RE::hkClass* XTE::RegisteredClass(const void*) noexcept
{
	return &reflectedClass;
}

bool XTE::RegisterTransitionClass(const void* templateTable, const void* runtimeTable) noexcept
{
	auto* builtin = RE::hkBuiltinTypeRegistry::GetSingleton();
	if (!builtin || !RE::hkMemoryRouter::GetInstancePtr()) return false;
	auto* classes = builtin->GetClassNameRegistry();
	auto* types = builtin->GetTypeInfoRegistry();
	auto* vtables = builtin->GetVtableClassRegistry();
	if (!classes || !types || !vtables || classes->Find(kClassName) || types->Find(kClassName)) return false;
	const auto* baseClass = classes->Find(kBaseName);
	const auto* baseType = types->Find(kBaseName);
	const auto* syncClass = classes->Find("hkbGeneratorSyncInfo");
	const auto nativeTable = REL::Relocation<const void*>(RE::hkbBlendingTransitionEffect::VTABLE[0]).get();
	if (!baseClass || !baseType || !syncClass || baseType->vtable != nativeTable ||
		baseClass->objectSize != sizeof(RE::hkbBlendingTransitionEffect) ||
		baseType->size != sizeof(RE::hkbBlendingTransitionEffect) ||
		syncClass->objectSize != sizeof(RE::hkbGeneratorSyncInfo) ||
		!baseType->finish || !baseType->cleanup || baseClass->GetSignature() != 0xfd8584feu) return false;

	// Havok 2010.2 includes these ignored members in the serialized class signature.
	using Member = RE::hkClassMember;
	const auto ignored = [](const char* name, Member::Type type, std::uint16_t offset, const RE::hkClass* klass = nullptr) {
		Member member{};
		member.m_name = name;
		member.m_class = klass;
		member.m_type = type;
		member.m_flags = Member::SERIALIZE_IGNORED;
		member.m_offset = offset;
		return member;
	};
	ignoredMembers = {
		ignored("fromGeneratorSyncInfo", Member::TYPE_STRUCT, 0x90, syncClass),
		ignored("worldFromModel", Member::TYPE_QSTRANSFORM, 0xE0),
		ignored("motion", Member::TYPE_QSTRANSFORM, 0x110),
		ignored("pose", Member::TYPE_ARRAY, 0x140),
		ignored("additivePose", Member::TYPE_ARRAY, 0x150),
		ignored("toGeneratorDuration", Member::TYPE_REAL, 0x160),
		ignored("isFromGeneratorActive", Member::TYPE_BOOL, 0x164),
		ignored("gotPose", Member::TYPE_BOOL, 0x165),
		ignored("gotAdditivePose", Member::TYPE_BOOL, 0x166)
	};
	reflectedClass.name = kClassName;
	reflectedClass.parent = baseClass;
	reflectedClass.objectSize = kSerializedSize;
	reflectedClass.declaredMembers = ignoredMembers.data();
	reflectedClass.numDeclaredMembers = static_cast<std::int32_t>(ignoredMembers.size());
	if (reflectedClass.GetSignature() != 0xd5ffe887u) return false;

	baseFinish = baseType->finish;
	typeInfo = {kClassName, "!hkbExtrapolatingTransitionEffect", Finish, baseType->cleanup, templateTable, kSerializedSize};
	builtin->RegisterType(&typeInfo, &reflectedClass);
	vtables->RegisterVtable(runtimeTable, &reflectedClass);
	return classes->Find(kClassName) == &reflectedClass && types->Find(kClassName) == &typeInfo;
}
