#pragma once

namespace RE
{
	class hkClass;
}

namespace XTE
{
	bool RegisterTransitionClass(const void* templateTable, const void* runtimeTable) noexcept;
	const RE::hkClass* RegisteredClass(const void*) noexcept;
}
