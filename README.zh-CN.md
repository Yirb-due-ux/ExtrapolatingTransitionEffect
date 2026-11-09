# Extrapolating Transition Effect

实验性 SKSE 插件，面向熟悉 Skyrim Behavior XML 和 HKX 的模组作者。仅支持 Skyrim SE 1.5.97。

已注册 `hkbExtrapolatingTransitionEffect`，可在 Behavior 状态机中启用。外推模式在前两帧采样 From。随后停用 From，用外推姿势与 To 混合。

## 设置

| Mode | 行为 |
| --- | --- |
| `stabilized` | 默认模式。在外推中加入姿态速度衰减和旋转连续性处理，用于减轻姿态扭曲和旋转跳变 |
| `extrapolated` | 使用 Havok 式外推，按前两帧求得的姿态增量继续推进，通常仅在短混合下有不错效果（0.1秒），相较正常混合有可忽略不计的性能提升 |
| `vanilla` | 保留 HKX 中的类和过渡引用，由 Skyrim 原版混合求值 |

## 在 Behavior 中启用

外推类沿用 `hkbBlendingTransitionEffect` 的 XML 字段。在目标过渡引用的效果对象上使用 `class="hkbExtrapolatingTransitionEffect"` 和 `signature="0xd5ffe887"`。如果多条边共用同一个效果，先复制一个独立对象并分配唯一编号，再修改目标边的引用。

下面是一个 0.4 秒的示例。内层 `name` 是图内节点名，可以按自己的命名习惯填写。

```xml
<hkobject name="#6100" class="hkbExtrapolatingTransitionEffect" signature="0xd5ffe887">
    <hkparam name="variableBindingSet">null</hkparam>
    <hkparam name="userData">0</hkparam>
    <hkparam name="name">ExampleMod</hkparam>
    <hkparam name="selfTransitionMode">SELF_TRANSITION_MODE_RESET</hkparam>
    <hkparam name="eventMode">EVENT_MODE_IGNORE_FROM_GENERATOR</hkparam>
    <hkparam name="duration">0.4</hkparam>
    <hkparam name="toGeneratorStartTimeFraction">0.000000</hkparam>
    <hkparam name="flags">FLAG_IGNORE_FROM_WORLD_FROM_MODEL</hkparam>
    <hkparam name="endMode">END_MODE_NONE</hkparam>
    <hkparam name="blendCurve">BLEND_CURVE_SMOOTH</hkparam>
</hkobject>
```

在目标 `hkbStateMachineTransitionInfo` 的 `transition` 字段引用该对象。

```xml
<hkparam name="transition">#6100</hkparam>
```

混合时长由效果的 `duration` 决定。若原图将 `duration` 绑定到变量，保留该绑定即可继续由变量控制时长。

## 从 XML 回包到 HKX

先合并 Behavior 补丁，再用支持该类的 `HavokBehaviorPostProcess.exe` 将完整的 Havok `hkpackfile` XML 写成 AMD64 HKX。
示例命令：

```powershell
.\HavokBehaviorPostProcess.exe --platformamd64 ".\mt_behavior.nemesis-normalized.xml" ".\mt_behavior.hkx"
```
