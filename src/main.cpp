#include <RE/Skyrim.h>
#include <REX/W32/KERNEL32.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "ClassRegistration.h"
#include "DensePoseTrack.h"
#include "Extrapolate.h"
#include "StabilizedPose.h"

#include <array>
#include <atomic>
#include <bitset>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string_view>
#include <system_error>

namespace logger = SKSE::log;

extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const SKSE::QueryInterface* skse, SKSE::PluginInfo* info)
{
    info->infoVersion = SKSE::PluginInfo::kVersion;
    info->name = "ExtrapolatingTransitionEffect";
    info->version = REL::Version{0, 3, 0, 0}.pack();
    return !skse->IsEditor() && skse->RuntimeVersion() == SKSE::RUNTIME_SSE_1_5_97;
}

namespace
{
    using Node = RE::hkbGenerator;
    using Effect = RE::hkbBlendingTransitionEffect;
    using Graph = RE::hkbBehaviorGraph;
    using Context = RE::hkbContext;
    using Output = RE::hkbGeneratorOutput;
    using TrackId = Output::TrackId;
    using SyncInfo = RE::hkbGeneratorSyncInfo;
    using Children = RE::hkbNode::ChildrenInfo;
    using ChildrenFlags = RE::hkbNode::GetChildrenFlags;
    using Transform = XTE::Transform;
    using XTE::QsTrack;
    using XTE::TrackValue;
    using Generate = void (*)(Node*, const Context&, const Output**, Output&, float);
    using Lifecycle = void (*)(Node*, const Context&);
    using Update = void (*)(Node*, const Context&, float);
    using Clone = RE::hkbNode* (*)(const RE::hkbNode*, Graph&);
    using Destroy = Node* (*)(Node*, std::uint32_t);
    using GetChildren = void (*)(Node*, ChildrenFlags, Children&);

    // Verified slots used by the registered transition class.
    enum class Slot : std::size_t
    {
        destroy = 0, getClassType = 1, activate = 4, update = 5, deactivate = 7,
        getChildren = 9, cloneNode = 0x0C, generate = 0x17, canRecycleOutput = 0x18,
        isDone = std::to_underlying(Effect::Slot::kIsDone),
        isSyncable = std::to_underlying(Effect::Slot::kIsSyncable),
        toStarted = std::to_underlying(Effect::Slot::kToStarted),
        useFromOnly = std::to_underlying(Effect::Slot::kUseFromOnly),
        fromSyncInfo = std::to_underlying(Effect::Slot::kFromSyncInfo),
        blendDuration = std::to_underlying(Effect::Slot::kBlendDuration),
        isFirstFrame = std::to_underlying(Effect::Slot::kIsFirstFrame),
    };
    constexpr std::size_t kEffectSize = sizeof(Effect);
    constexpr std::size_t kSlotCount = std::to_underlying(Slot::isFirstFrame) + 1;
    constexpr auto kMaxBones = XTE::kMaxBones;

    static_assert(sizeof(RE::hkQsTransform) == sizeof(Transform));
    static_assert(alignof(RE::hkQsTransform) == alignof(Transform));
    static_assert(offsetof(RE::hkQsTransform, translation) == offsetof(Transform, translation));
    static_assert(offsetof(RE::hkQsTransform, rotation) == offsetof(Transform, rotation));
    static_assert(offsetof(RE::hkQsTransform, scale) == offsetof(Transform, scale));

    struct Stabilization
    {
        std::array<Transform, kMaxBones> predicted;
        std::array<DirectX::XMFLOAT4, kMaxBones> rotations;
        std::array<DirectX::XMFLOAT4, kMaxBones> rotationPath{};
        std::array<float, kMaxBones> stepRates, halfLives;
        float sourceSampleTime{}, goalAge{-1}, lastRotationAge{-1};
    };

    struct State
    {
        std::array<Transform, kMaxBones> pose, delta;
        Transform world{XTE::identity}, motion{XTE::identity};
        SyncInfo fromSync{};
        std::unique_ptr<Stabilization> stable;
        std::size_t boneCount{};
        float prefix{}, blendDuration{};
        bool enabled{}, fromActive{true}, first{}, ready{};
        // Pad the scalar tail to the alignment of the pose storage.
        std::byte tailPadding[alignof(Transform) - 2 * sizeof(float) - 4 * sizeof(bool)]{};
    };
    static_assert(std::is_nothrow_destructible_v<State>);
    static_assert(kEffectSize % alignof(State) == 0 && kEffectSize + sizeof(State) < 0x7FFF);

    std::array<std::uintptr_t, kSlotCount> chainedSlots;
    std::array<std::uintptr_t, kSlotCount + 1> classTemplateTable;
    std::array<std::uintptr_t, kSlotCount + 1> classRuntimeTable;
    std::atomic_bool tablesReady{false};

    template <class Fn>
    Fn Chained(Slot slot) noexcept
    {
        return reinterpret_cast<Fn>(chainedSlots[std::to_underlying(slot)]);
    }

    void SetVtable(void* object, const std::uintptr_t* table) noexcept
    {
        std::memcpy(object, &table, sizeof(table));
    }

    State& Extension(Effect* effect) noexcept
    {
        return *std::launder(reinterpret_cast<State*>(reinterpret_cast<std::byte*>(effect) + kEffectSize));
    }

    template <spdlog::level::level_enum Level = spdlog::level::info, class... Args>
    void Log(fmt::format_string<Args...> format, Args&&... args) noexcept
    {
        try { spdlog::log(Level, format, std::forward<Args>(args)...); }
        catch (const std::exception&) {}
    }

    bool PlayerGraph(const Context& context) noexcept
    {
        if (!context.character) return false;
        const auto* graph = RE::BShkbAnimationGraph::FromCharacterInstance(*context.character);
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player && graph->holder == player;
    }

    const RE::hkbNodeInfo* ActiveNode(const Graph* graph, const RE::hkbNode* node) noexcept
    {
        if (!graph || !graph->activeNodes || !node) return nullptr;
        for (const auto& info : *graph->activeNodes) {
            if (info.nodeTemplate == node || info.nodeClone == node) return &info;
        }
        return nullptr;
    }

    const SyncInfo* ActiveSync(const Graph& graph, const RE::hkbNode* node) noexcept
    {
        auto* info = ActiveNode(&graph, node);
        if (!info) return nullptr;
        if (info->useSyncInfoIndex) {
            const auto index = info->syncInfoIndex;
            if (index < 0 || index >= graph.activeNodes->size()) return nullptr;
            info = &(*graph.activeNodes)[index];
        }
        return &info->syncInfo;
    }

    enum class Mode { vanilla, extrapolated, stabilized };
    Mode configuredMode{Mode::stabilized};

    Mode ParseMode(std::wstring_view value) noexcept
    {
        if (value == L"stabilized") return Mode::stabilized;
        if (value == L"extrapolated") return Mode::extrapolated;
        if (value != L"vanilla") Log<spdlog::level::warn>("XTE invalid transition mode. Using vanilla.");
        return Mode::vanilla;
    }

    Mode LoadMode() noexcept
    {
        try {
            const auto path = std::filesystem::absolute(L"Data/SKSE/Plugins/ExtrapolatingTransitionEffect/settings.ini");
            if (!std::filesystem::exists(path)) {
                const std::filesystem::path legacyPath{"Data/SKSE/Plugins/ExtrapolatingTransitionEffect.mode"};
                if (!std::filesystem::exists(legacyPath)) return Mode::stabilized;
                std::wifstream legacy(legacyPath);
                std::wstring value;
                if (legacy >> value) return ParseMode(value);
            } else {
                constexpr std::wstring_view missingMode = L"\x01";
                std::array<wchar_t, 32> value{};
                const auto count = REX::W32::GetPrivateProfileStringW(L"General", L"Mode", missingMode.data(),
                    value.data(), static_cast<std::uint32_t>(value.size()), path.c_str());
                const std::wstring_view mode{value.data(), count};
                if (mode != missingMode) return ParseMode(mode);
                const std::error_code error{static_cast<int>(REX::W32::GetLastError()), std::system_category()};
                if (error == std::errc::no_such_file_or_directory) return Mode::stabilized;
            }
        } catch (const std::exception&) {
            // Path and stream exceptions use the same fallback as an unsuccessful read.
        }
        Log<spdlog::level::warn>("XTE settings read failed. Using vanilla.");
        return Mode::vanilla;
    }

    RE::hkbNode* CloneRegistered(const RE::hkbNode* source, Graph& graph) noexcept
    {
        auto* router = RE::hkMemoryRouter::GetInstancePtr();
        if (!router || !router->Heap) SKSE::stl::report_and_fail("ETE clone requires the Havok TLS heap");
        constexpr auto size = static_cast<std::uint16_t>(kEffectSize + sizeof(State));
        void* storage = router->Heap->BlockAlloc(size);
        if (!storage) return Chained<Clone>(Slot::cloneNode)(source, graph);
        // Storage has no live Havok object until the engine copy constructor runs.
        std::memcpy(static_cast<std::byte*>(storage) + offsetof(RE::hkReferencedObject, memSizeAndFlags), &size, sizeof(size));
        auto* effect = Effect::CopyConstruct(storage, source);
        effect->memSizeAndFlags = size;
        auto* state = std::construct_at(reinterpret_cast<State*>(static_cast<std::byte*>(storage) + kEffectSize));
        state->enabled = configuredMode != Mode::vanilla;
        SetVtable(effect, classRuntimeTable.data() + 1);
        return effect;
    }

    Node* DestroyEffect(Effect* effect, std::uint32_t flags) noexcept
    {
        auto& state = Extension(effect);
        std::destroy_at(&state);
        // This clone was allocated by the Havok heap and carries its actual size in the native header.
        return Chained<Destroy>(Slot::destroy)(effect,
            flags & ~std::to_underlying(Effect::DeletingDestructorFlag::kFreeFixedSize));
    }

    void ActivateEffect(Effect* effect, const Context& context) noexcept
    {
        Chained<Lifecycle>(Slot::activate)(effect, context);
        auto& state = Extension(effect);
        const auto mode = configuredMode;
        state.enabled = mode != Mode::vanilla && PlayerGraph(context) && effect->fromGenerator && effect->toGenerator;
        state.stable.reset(state.enabled && mode == Mode::stabilized ? new (std::nothrow) Stabilization : nullptr);
        if (state.enabled && mode == Mode::stabilized && !state.stable) {
            state.enabled = false;
            Log<spdlog::level::warn>("XTE stabilized pose allocation failed");
        }
        state.fromActive = true;
        state.first = state.ready = false;
        state.boneCount = 0;
        state.prefix = state.blendDuration = 0;
        state.world = state.motion = XTE::identity;
        state.fromSync = {};
    }

    void DeactivateEffect(Effect* effect, const Context& context) noexcept
    {
        auto& state = Extension(effect);
        Chained<Lifecycle>(Slot::deactivate)(effect, context);
        state.stable.reset();
    }

    void UpdateEffect(Effect* effect, const Context& context, float dt) noexcept
    {
        Chained<Update>(Slot::update)(effect, context, dt);
        auto& state = Extension(effect);
        if (state.enabled && state.fromActive && (state.ready || effect->timeRemaining < 0) && context.character) {
            if (auto* graph = context.character->behaviorGraph.get()) graph->updateActiveNodes = true;
        }
    }

    void EnumerateChildren(Effect* effect, ChildrenFlags flags, Children& children) noexcept
    {
        Chained<GetChildren>(Slot::getChildren)(effect, flags, children);
        auto& state = Extension(effect);
        constexpr auto activeOnly = RE::hkbNode::GetChildrenFlagBits::kActiveOnly;
        auto& list = children.childInfos;
        if (!state.enabled || !flags.any(activeOnly) || list.size() != 2) return;
        // Generators-only queries can run before Update requests the active graph rebuild.
        if (flags == activeOnly && state.fromActive && (state.ready || effect->timeRemaining < 0)) {
            if (state.ready && effect->timeRemaining > 0) {
                state.prefix = effect->timeInTransition;
                state.blendDuration = effect->duration - state.prefix;
                effect->timeInTransition = 0.0f;
                effect->timeRemaining = state.blendDuration;
            }
            state.fromActive = false;
        }
        if (state.fromActive) list[0] = list[1];
        list[0].reset = children.reset;
        list._size = 1;
    }

    bool CanRecycle(Effect* effect) noexcept
    {
        const auto& state = Extension(effect);
        return state.enabled ? state.fromActive : Chained<bool (*)(Node*)>(Slot::canRecycleOutput)(effect);
    }

    bool IsDone(Effect* effect) noexcept
    {
        const auto& state = Extension(effect);
        if (!state.enabled) return Chained<bool (*)(Node*)>(Slot::isDone)(effect);
        return effect->timeInTransition > 0 && (effect->fromGenerator == effect->toGenerator || (!state.fromActive && effect->timeRemaining <= 0));
    }

    bool IsSyncable(Effect* effect, Graph& graph) noexcept
    {
        const auto& state = Extension(effect);
        if (!state.enabled) return Chained<bool (*)(Node*, Graph&)>(Slot::isSyncable)(effect, graph);
        const auto* from = state.fromActive ? ActiveSync(graph, effect->fromGenerator) : &state.fromSync;
        if (!from || from->duration == 0) return false;
        if (state.fromActive) return true;
        const auto* to = ActiveSync(graph, effect->toGenerator);
        return to && to->duration != 0;
    }

    bool ToStarted(Effect* effect) noexcept
    {
        const auto& state = Extension(effect);
        if (!state.enabled) return Chained<bool (*)(Node*)>(Slot::toStarted)(effect);
        return !state.fromActive && (effect->timeInTransition == 0 || !state.ready);
    }

    bool UseFromOnly(Effect* effect) noexcept
    {
        const auto& state = Extension(effect);
        if (!state.enabled) return Chained<bool (*)(Node*)>(Slot::useFromOnly)(effect);
        return state.fromActive || effect->timeInTransition == 0 || !state.ready;
    }

    const SyncInfo* FromSyncInfo(Effect* effect, Graph& graph) noexcept
    {
        const auto native = Chained<const SyncInfo* (*)(Node*, Graph&)>(Slot::fromSyncInfo);
        auto& state = Extension(effect);
        if (!state.enabled) return native(effect, graph);
        if (state.fromActive && ActiveNode(&graph, effect->fromGenerator)) {
            std::memcpy(&state.fromSync, native(effect, graph), sizeof(SyncInfo));
        }
        return &state.fromSync;
    }

    float BlendDuration(Effect* effect) noexcept
    {
        const auto& state = Extension(effect);
        return !state.enabled || state.fromActive ? effect->duration : state.blendDuration;
    }

    bool IsFirstFrame(Effect* effect) noexcept
    {
        const auto& state = Extension(effect);
        if (!state.enabled) return Chained<bool (*)(Node*)>(Slot::isFirstFrame)(effect);
        return state.fromActive && effect->timeInTransition == 0;
    }

    void CaptureSource(State& state, const Output& input, float elapsed, float timeOffset) noexcept
    {
        const auto pose = QsTrack(input, TrackId::kPose);
        auto* stable = state.stable.get();
        if (stable && (timeOffset != 0 || !std::isfinite(elapsed) || !pose.count || pose.fraction <= 0)) return;
        if (!state.first) {
            if (stable) {
                for (std::size_t i = 0; i < pose.count; ++i) if (!pose.Has(i)) return;
                stable->sourceSampleTime = elapsed;
            }
            state.world = TrackValue(QsTrack(input, TrackId::kWorldFromModel));
            state.first = true;
            state.boneCount = pose.count;
            if (pose.count) std::memcpy(state.pose.data(), pose.data, pose.count * sizeof(Transform));
            return;
        }
        if (state.ready) return;
        state.motion = TrackValue(QsTrack(input, TrackId::kExtractedMotion));
        if (!pose.count || pose.count != state.boneCount) return;
        const float interval = stable ? elapsed - stable->sourceSampleTime : 0;
        if (stable && interval <= 1e-6f) return;
        for (std::size_t i = 0; i < pose.count; ++i) {
            if (stable) {
                if (!pose.Has(i) || !XTE::ValidPose(state.pose[i]) || !XTE::ValidPose(pose.At(i))) return;
            } else {
                const auto& s = state.pose[i].scale;
                if (s.x == 0 || s.y == 0 || s.z == 0) return;
            }
        }
        for (std::size_t i = 0; i < pose.count; ++i) {
            auto next = pose.At(i);
            if (stable) {
                state.delta[i] = XTE::StabilizedDelta(state.pose[i], next);
                stable->stepRates[i] = XTE::PredictionStepRate(state.delta[i], next, interval);
            } else state.delta[i] = XTE::Relative(state.pose[i], next);
            state.pose[i] = next;
        }
        if (stable) stable->sourceSampleTime = elapsed;
        state.ready = true;
    }

    void GenerateTo(Effect* effect, const Context& context, const Output& to, Output& output, float timeOffset) noexcept
    {
        auto& state = Extension(effect);
        const auto sourcePose = QsTrack(to, TrackId::kPose);
        const auto destination = QsTrack(output, TrackId::kPose);
        if (!state.ready || destination.count != state.boneCount || sourcePose.count != state.boneCount) return;
        const float duration = effect->timeInTransition + effect->timeRemaining;
        float weight = duration != 0 ? (std::min)(effect->timeInTransition + timeOffset, duration) / duration : 0;
        const Transform* predicted = state.pose.data();
        std::bitset<kMaxBones> replaceRotation;
        auto* stable = state.stable.get();
        if (stable) {
            const float clock = state.prefix + effect->timeInTransition + timeOffset - stable->sourceSampleTime;
            if (!std::isfinite(clock) || !std::isfinite(weight)) return;
            const float age = (std::max)(0.0f, clock);
            weight = std::clamp(weight, 0.0f, 1.0f);
            const bool commit = timeOffset == 0 && age > stable->lastRotationAge;
            const auto* graph = context.character ? context.character->behaviorGraph.get() : nullptr;
            const auto* toSync = commit && stable->goalAge < 0 && weight > 0 && sourcePose.fraction > 0 && graph ?
                ActiveSync(*graph, effect->toGenerator) : nullptr;
            const bool latchGoal = toSync && toSync->duration > 0;
            if (latchGoal) stable->goalAge = age;
            for (std::size_t i = 0; i < state.boneCount; ++i) {
                const bool validTarget = sourcePose.Has(i);
                const auto target = validTarget ? sourcePose.At(i) : XTE::identity;
                const auto& delta = state.delta[i];
                const auto& seed = state.pose[i];
                if (latchGoal) stable->halfLives[i] = validTarget && XTE::ValidPose(target) ?
                    XTE::TargetHalfLife(delta, seed, target, stable->stepRates[i]) : XTE::kPredictionHalfLife;
                const float halfLife = stable->goalAge < 0 ? XTE::kPredictionHalfLife : stable->halfLives[i];
                const auto steps = stable->stepRates[i] * XTE::DampedSeconds(age, stable->goalAge, halfLife);
                stable->predicted[i] = XTE::AdvanceFraction(delta, seed, steps);
                if (validTarget && XTE::ContinuousRotation(stable->predicted[i], target, weight, stable->rotationPath[i], commit, stable->rotations[i])) {
                    replaceRotation.set(i);
                }
            }
            if (commit) stable->lastRotationAge = age;
            predicted = stable->predicted.data();
        } else {
            for (std::size_t i = 0; i < state.boneCount; ++i) state.pose[i] = XTE::Compose(state.delta[i], state.pose[i]);
        }
        auto mask = sourcePose.Mask();
        auto outMask = destination.Mask();
        auto* destinationPose = reinterpret_cast<RE::hkQsTransform*>(destination.data);
        Output::BlendPoses(reinterpret_cast<const RE::hkQsTransform*>(predicted), &mask,
            reinterpret_cast<const RE::hkQsTransform*>(sourcePose.data), &mask,
            static_cast<std::int32_t>(state.boneCount), weight, destinationPose, &outMask);
        if (stable && replaceRotation.any()) {
            for (std::size_t i = 0; i < state.boneCount; ++i) {
                if (replaceRotation.test(i) && destination.Has(i)) {
                    std::memcpy(&destinationPose[i].rotation, &stable->rotations[i], sizeof(Transform::rotation));
                }
            }
        }

        if (const auto* world = context.character ? context.character->worldFromModel : nullptr) {
            Transform value;
            std::memcpy(&value, world, sizeof(value));
            state.world = XTE::Compose(value, state.motion);
        }
        const auto flags = effect->flags;
        if (flags.any(Effect::FlagBits::kIgnoreFromWorldFromModel)) return;
        if (const auto track = QsTrack(output, TrackId::kWorldFromModel); track.count) {
            const auto value = flags.any(Effect::FlagBits::kIgnoreToWorldFromModel) ? state.world :
                XTE::Blend(state.world, TrackValue(QsTrack(to, TrackId::kWorldFromModel)), weight);
            std::memcpy(track.data, &value, sizeof(value));
        }
        if (!flags.any(Effect::FlagBits::kIgnoreToWorldFromModel)) {
            if (const auto track = QsTrack(output, TrackId::kExtractedMotion); track.count) {
                const auto value = XTE::Blend(state.motion, TrackValue(QsTrack(to, TrackId::kExtractedMotion)), weight);
                std::memcpy(track.data, &value, sizeof(value));
            }
        }
    }

    void GenerateEffect(Effect* effect, const Context& context, const Output** children, Output& output, float timeOffset) noexcept
    {
        auto& state = Extension(effect);
        if (!state.enabled || effect->fromGenerator == effect->toGenerator) {
            Chained<Generate>(Slot::generate)(effect, context, children, output, timeOffset);
            return;
        }
        if (!children || !children[0]) return;

        const auto& child = *children[0];
        if (state.fromActive) CaptureSource(state, child, effect->timeInTransition, timeOffset);
        if (child.tracks != output.tracks) output.CopyTracksFrom(child);
        if (!state.fromActive) GenerateTo(effect, context, child, output, timeOffset);
    }

    void PrepareTransitionTables() noexcept
    {
        const REL::Relocation<const std::uintptr_t*> transition{Effect::VTABLE[0]};
        const auto* current = transition.get();
        std::ranges::copy(current, current + kSlotCount, chainedSlots.begin());
        std::ranges::copy(current - 1, current + kSlotCount, classTemplateTable.begin());
        classRuntimeTable = classTemplateTable;
        const auto set = [](Slot slot, auto target) {
            classRuntimeTable[std::to_underlying(slot) + 1] = reinterpret_cast<std::uintptr_t>(target);
        };
        set(Slot::destroy, DestroyEffect);
        set(Slot::getClassType, XTE::RegisteredClass);
        set(Slot::activate, ActivateEffect);
        set(Slot::update, UpdateEffect);
        set(Slot::deactivate, DeactivateEffect);
        set(Slot::getChildren, EnumerateChildren);
        set(Slot::generate, GenerateEffect);
        set(Slot::canRecycleOutput, CanRecycle);
        set(Slot::isDone, IsDone);
        set(Slot::isSyncable, IsSyncable);
        set(Slot::toStarted, ToStarted);
        set(Slot::useFromOnly, UseFromOnly);
        set(Slot::fromSyncInfo, FromSyncInfo);
        set(Slot::blendDuration, BlendDuration);
        set(Slot::isFirstFrame, IsFirstFrame);

        classTemplateTable[std::to_underlying(Slot::getClassType) + 1] =
            reinterpret_cast<std::uintptr_t>(XTE::RegisteredClass);
        classTemplateTable[std::to_underlying(Slot::cloneNode) + 1] =
            reinterpret_cast<std::uintptr_t>(CloneRegistered);

        tablesReady.store(true, std::memory_order_release);
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (message->type == SKSE::MessagingInterface::kPostLoad) {
            PrepareTransitionTables();
        }
        if (message->type == SKSE::MessagingInterface::kDataLoaded && tablesReady.load(std::memory_order_acquire)) {
            if (XTE::RegisterTransitionClass(classTemplateTable.data() + 1, classRuntimeTable.data() + 1)) {
                Log("XTE registry ready class=hkbExtrapolatingTransitionEffect signature=0xd5ffe887");
            } else Log<spdlog::level::err>("XTE class registry validation failed");
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    if (skse->IsEditor() || skse->RuntimeVersion() != SKSE::RUNTIME_SSE_1_5_97) return false;
    const auto directory = logger::log_directory();
    if (!directory) return false;
    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*directory / "ExtrapolatingTransitionEffect.log").string(), true);
        auto log = std::make_shared<spdlog::logger>("ExtrapolatingTransitionEffect", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
    } catch (const spdlog::spdlog_ex&) {
        return false;
    }
    SKSE::Init(skse, false);
    configuredMode = LoadMode();
    Log("XTE 0.3.0 Skyrim {}", skse->RuntimeVersion().string());
    const auto messaging = SKSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener("SKSE", OnMessage);
}
