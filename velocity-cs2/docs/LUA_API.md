# Velocity Pro Lua API

## v5 底层查询、定时器与只读状态

当前版本：

```lua
print(velocity.version) -- 5
```

v5 保留 v4 的所有接口，并新增命中箱可见性查询、最佳可见骨骼查询、脚本私有异步定时器和模块内部状态快照。v4 已加入的骨骼、碰撞箱、扩展射线、静默角度、命令开火、实体底层快照和细粒度事件仍然可用。实体索引 `-1` 在这些底层接口中表示本地玩家。

### v5 快速索引

```lua
velocity.trace.hitbox(start, target_index, hitbox_index
    [, skip_index, mask, layer])                         -- trace_result 或 nil
velocity.trace.most_visible_bone(start, target_index
    [, candidates, skip_index, mask, layer])             -- bone 或 nil

velocity.schedule(delay, callback)                       -- task_id 或 nil
velocity.cancel_schedule(task_id)                        -- boolean

velocity.modules.state(name)                             -- 只读 table 或 nil
```

`start`、位置和角度均使用世界坐标；`delay` 的单位是秒。所有返回对象都是调用时刻的快照，后续帧不会自动更新。

### 命令处理阶段

命令回调按以下顺序执行：

1. `create_move_pre`：反 subtick 和预测前置状态捕获后、内置功能运行前。
2. `create_move`：所有内置功能运行后，兼容原有脚本。
3. `create_move_post`：所有普通脚本命令回调之后、命令序列化之前。

三种回调都接收同一个 `command` 类型：

```lua
velocity.on("create_move_pre", function(cmd)
    -- 读取原始命令或抢先修改
end)

velocity.on("create_move_post", function(cmd)
    -- 最终修改仍会写入本 tick 命令
end)
```

新增命令方法：

```lua
cmd:set_silent_angles(angles)   -- 只改用户命令，不转动镜头，并保持原移动方向
cmd:set_camera_angles(angles)   -- 转动本地镜头
cmd:fire([secondary])           -- 按下 attack；secondary=true 使用 attack2
cmd:stop_firing()               -- 松开 attack 和 attack2
```

也可在命令回调中使用全局输入入口：

```lua
velocity.input.set_view_angles(angles)     -- 可见镜头角度
velocity.input.set_silent_angles(angles)   -- boolean，是否存在当前命令
velocity.input.fire([secondary])           -- boolean
velocity.input.stop_firing()               -- boolean
```

静默瞄准并开火示例：

```lua
velocity.on("create_move_post", function(cmd)
    local target = velocity.entity.players()[1]
    if not target or not target.alive then return end

    local head = velocity.entity.bone(target.index, 7)
    if not head or not head.valid then return end

    local eye = velocity.player.local().eye_position
    if velocity.trace.visible(eye, head.position, target.index) then
        cmd:set_silent_angles(velocity.math.angle_to(eye, head.position))
        cmd:fire()
    end
end)
```

### 骨骼与碰撞箱

```lua
velocity.entity.bone(entity_index, bone_id) -- bone 或 nil
velocity.entity.skeleton(entity_index)      -- bone[]，仅返回有效骨骼
velocity.entity.hitboxes(entity_index)      -- hitbox[]
```

常用骨骼 ID：`pelvis=1`、`spine_1=2`、`spine_2=3`、`spine_3=4`、`neck=6`、`head=7`、`left_hand=11`、`right_hand=15`、`left_foot=19`、`right_foot=22`、`spine_4=23`。

`bone` 字段：`id`、`position`、`rotation`（`quaternion`）、`scale`、`valid`。v5 还增加 `fraction`、`fov`、`hitbox_index`、`hitgroup`、`visible`；这些附加字段主要由 `trace.most_visible_bone()` 填充，普通 `entity.bone()`/`entity.skeleton()` 快照中的附加字段保持默认值。

`hitbox` 字段：`index`、`bone`、`mins`、`maxs`、`radius`、`hitgroup`、`hitgroup_name`、`shape_type`、`translation_only`。`mins` 和 `maxs` 是骨骼局部空间坐标；结合对应骨骼的位置和旋转使用。

### 扩展射线

旧的两参数 `line`、四参数 `hull` 和三参数 `visible` 调用保持兼容。可选参数中的 `skip_index` 默认为本地玩家，传 `-1` 也明确表示本地玩家；默认 `mask` 为 `0x1c3003`，默认 `layer` 为 `4`。

```lua
velocity.trace.line(start, finish [, skip_index, mask, layer])
velocity.trace.hull(start, finish, mins, maxs [, skip_index, mask, layer])
velocity.trace.sphere(start, finish, radius [, skip_index, mask, layer])
velocity.trace.to_entity(start, finish, target_index [, skip_index, mask, layer])
velocity.trace.visible(start, finish, target_index [, skip_index, mask])
velocity.trace.hitbox(start, target_index, hitbox_index [, skip_index, mask, layer])
velocity.trace.most_visible_bone(start, target_index [, candidates, skip_index, mask, layer])
```

`trace_result` 字段：`fraction`、`start`、`end`、`position`、`normal`、`hit_index`、`contents`、`hitbox_index`、`bone`、`hitgroup`、`visible`、`hit`、`all_solid`。后三个目标元数据主要由 `trace.hitbox()` 填充。`to_entity` 或 `hitbox` 在目标/命中箱无法解析时返回 `nil`。

```lua
local result = velocity.trace.sphere(from, to, 4.0)
if result.hit then
    print(result.hit_index, result.fraction, result.contents)
end
```

#### `trace.hitbox()`

`trace.hitbox()` 读取目标当前模型的命中箱和对应骨骼，将命中箱局部空间的 `(mins + maxs) / 2` 通过骨骼旋转和平移转换为世界空间中心，然后从 `start` 向该中心执行完整场景射线。墙体、实体和其他符合 filter 的碰撞物都会参与判断。

```lua
local eye = velocity.player.local().eye_position
local result = velocity.trace.hitbox(eye, enemy.index, 0)
if result and result.visible then
    print("hitbox center visible", result.bone, result.hitgroup)
end
```

返回值中的 `hitbox_index` 是请求的命中箱编号，`bone` 是该命中箱绑定的骨骼编号，`hitgroup` 是当前构建根据命中箱映射出的命中组。它们是目标元数据，不应理解为引擎一定在该命中箱表面产生了物理命中。`visible` 在射线直接命中目标实体，或射线已经走过 97% 路径时为 `true`。

该函数测试的是命中箱中心的直线可见性，不计算子弹穿透、伤害、扩散或命中率。命中箱的一部分露出而中心被挡住时，结果仍可能是不可见。

#### `trace.most_visible_bone()`

```lua
local bone = velocity.trace.most_visible_bone(eye, enemy.index)
local torso = velocity.trace.most_visible_bone(
    eye, enemy.index, {7, 6, 4, 2, 1})

-- 要跳过候选表但继续传后续可选参数，需要显式传 nil。
local custom = velocity.trace.most_visible_bone(
    eye, enemy.index, nil, -1, 0x1c3003, 4)
```

`candidates` 是按 Lua 数组形式传入的骨骼 ID 表。省略、传 `nil`、传空表，或表中没有合法的 `0..255` 整数时，使用默认候选 `{7, 6, 2, 1}`（头、颈、上身、骨盆）。函数对每个有效骨骼执行完整场景射线，只保留直接命中目标或射线通过 97% 路径的骨骼，再选择相对当前输入视角 FOV 最小的一个。

成功时返回扩展 `bone` 快照，其中 `visible=true`、`valid=true`，并填充 `fraction` 与 `fov`。没有目标、所有候选骨骼无效或全部被遮挡时返回 `nil`。选择规则只比较可见性与 FOV，不比较伤害、命中箱面积、穿透能力或武器散布。

#### Mask、Contents 与 Layer

`mask` 是传给引擎 trace filter 的碰撞内容位集合，用于决定哪些类别的表面或实体能够阻挡/命中射线。它不是武器 ID、伤害类型、hitgroup，也不是“是否可穿透”的开关。

- 默认值是 `0x1c3003`，所有 trace API 在未传 `mask` 时使用该值。
- Lua 数值可直接使用十进制或十六进制整数，例如 `0x1c3003`。
- 需要组合已确认的内容位时可用 Lua 位运算，例如 `mask_a | mask_b`；移除位可用 `mask & ~flag`。
- `trace_result.contents` 是本次命中结果报告的内容标志，不是输入 `mask` 的回显。输入 mask 表示“允许查询什么”，结果 contents 表示“实际命中了什么”。
- 各单独 bit 的名称和语义取决于当前 CS2/Source 2 构建。本文只保证默认组合及传参行为，不建议脚本硬编码未经当前构建验证的内容位名称。

`layer` 是 filter 的碰撞层，取值会限制到 `0..255`，默认值为 `4`。它与 `mask` 是两个独立筛选维度：相同 mask 在不同 layer 下可能得到不同结果，修改 mask 也不会自动改变 layer。`trace.visible()` 沿用内部可见性查询，只接受可选 `skip_index` 和 `mask`，不接受 `layer`。

### 扩展实体快照

所有 `entity` 快照新增以下字段：

```text
eye_position      vec3     眼睛世界坐标
armor             integer  护甲值，非玩家为 0
flags             integer  实体状态标志
simulation_time   number   模拟时间
scoped            boolean  是否开镜，非玩家为 false
```

快照仍然是只读时点数据，不会自行更新。下一帧需要重新调用 `by_index`、`players` 或 `player.local`。

### 游戏事件

所有支持的游戏事件既会触发通用 `game_event` 回调，也会触发同名回调。通用回调适合集中分发，同名回调适合只关心一个事件的脚本。

```lua
velocity.on("game_event", function(event)
    print(event.name)
end)

velocity.on("player_hurt", function(event)
    local damage = event:get_int("dmg_health")
    local health = event:get_int("health")
    local victim = event:pawn_index("userid")
    local from_local = event:is_local("attacker")
end)

velocity.on("bullet_impact", function(event)
    local position = event:get_vector() -- 默认读取 x/y/z
end)
```

事件对象方法：

```lua
event:get_int(key [, fallback])
event:get_float(key [, fallback])
event:get_vector([x_key, y_key, z_key])
event:pawn_index([key])                 -- 默认 userid，不存在返回 -1
event:is_local([key])                   -- 默认 userid
```

支持的细粒度事件：

```text
weapon_fire, weapon_reload, item_purchase, item_pickup,
player_spawn, player_blind, player_footstep, bullet_impact,
player_hurt, player_death, round_prestart, round_start,
round_freeze_end, round_end, bomb_beginplant, bomb_abortplant,
bomb_planted, bomb_begindefuse, bomb_abortdefuse, bomb_defused,
bomb_exploded, grenade_thrown, inferno_startburn, inferno_expire
```

部分事件只会在对应游戏模式或地图中注册并触发。事件回调在游戏事件分发线程同步执行，不要在其中进行长时间阻塞操作。

### 异步定时器

```lua
local task_id = velocity.schedule(0.25, function()
    velocity.log.info("250 ms elapsed")
end)

if task_id and should_cancel then
    local cancelled = velocity.cancel_schedule(task_id)
end
```

`velocity.schedule(delay, callback)` 创建脚本私有的一次性任务。`delay` 使用秒，可以是浮点数；合法范围为 `0..86400`（最长 24 小时）。成功返回当前脚本内唯一的正整数任务 ID；回调无效、delay 为负数、NaN 或无穷大时返回 `nil`。大于 86400 秒的合法延迟会限制为 86400 秒。

定时器不会创建后台线程，也不会从其他线程直接调用 Lua。到期任务在后续渲染帧开始时、该帧 `paint` 回调之前，在渲染线程上执行。因此：

- 延迟是“至少等待多久”，不是实时精确定时；实际回调时间受帧率、游戏暂停、窗口状态和渲染是否继续影响。
- `delay=0` 仍然异步，最早在下一次定时器泵出时运行，不会在 `schedule()` 调用栈内立即执行。
- 回调是一次性的，执行前会从队列移除。回调内可以继续调用 `schedule()`。
- 单个脚本一次泵出最多运行 1024 个到期回调，防止零延迟回调无限自调度阻塞一帧；剩余任务留到下一帧。
- 定时器回调抛出的 Lua 错误会进入脚本错误状态和 Lua 控制台，但不会让已执行的一次性任务重新入队。
- 卸载、重载或脚本加载失败时，该脚本的全部待处理任务都会被清除，任务 ID 也会重置。

`velocity.cancel_schedule(id)` 只检查调用它的当前脚本。任务仍在队列中时删除并返回 `true`；ID 不存在、任务已执行/取消，或属于另一个脚本时返回 `false`。取消一个正在执行的回调没有意义，因为任务在调用回调前已经出队。

需要重复定时器时，在回调尾部显式重新安排：

```lua
local running = true
local function poll()
    if not running then return end
    local state = velocity.modules.state("ragebot")
    if state then
        velocity.log.info("rage target: " .. tostring(state.target_index))
    end
    velocity.schedule(0.5, poll)
end

velocity.schedule(0.5, poll)
```

## 脚本目录与加载

DLL 初始化后会创建：

```text
C:\velocity-pro\luas
```

将 `.lua` 文件放入该目录，在菜单的 `Lua` 页面点击 `refresh` 后即可加载。每个脚本拥有独立的 Lua 状态、回调和 UI 状态。卸载脚本会销毁该脚本创建的 UI 控件，但脚本通过 `velocity.modules` 或 `velocity.config` 对现有模块做出的修改不会自动回滚。

## 回调

回调可以使用全局函数，也可以通过 `velocity.on` 注册。

### 线程与执行顺序

同一个脚本的 `paint`、`menu`、命令、游戏事件和定时器回调共享同一个 Lua VM。游戏本身可能从渲染线程、命令线程或事件分发线程进入这些回调，但 Velocity 会在引擎边界统一串行化所有 Lua 操作：任意时刻最多只有一个 Lua 回调执行，不会并发访问脚本状态。不同脚本也由同一脚本引擎依次调用，不提供并行执行保证。

回调仍在触发它的原始线程同步执行；串行化不会把命令或游戏事件自动转发到渲染线程。因此不要在任何回调中等待另一个 Lua 回调、进行长时间阻塞，或使用依赖“两个 Lua 回调同时运行”的逻辑。需要延迟工作时使用 `velocity.schedule()`，它会在后续渲染帧执行。

| 事件               | 全局函数                      | 参数        | 调用时机         |
| ---------------- | ------------------------- | --------- | ------------ |
| `paint`          | `on_paint()`              | 无         | 每个渲染帧        |
| `menu`           | `on_menu()`               | 无         | 绘制 Lua 菜单页面时 |
| `create_move_pre` | `on_create_move_pre(command)` | `command` | 内置功能运行前       |
| `create_move`    | `on_create_move(command)` | `command` | 内置功能运行后       |
| `create_move_post` | `on_create_move_post(command)` | `command` | 命令序列化前       |
| `level_init`     | `on_level_init(map_name)` | string    | 地图开始加载       |
| `level_shutdown` | `on_level_shutdown()`     | 无         | 地图卸载         |

```lua
velocity.on("paint", function()
    velocity.draw.text(velocity.vec2(20, 20), "Lua v" .. velocity.version,
        velocity.color(255, 255, 255))
end)
```

## 基础类型

### `vec2`

```lua
local value = velocity.vec2(10, 20)
```

字段：`x`、`y`。

方法：`length()`、`normalized()`。

运算：`+`、`-`、`* number`。

### `vec3`

字段：`x`、`y`、`z`。

方法：`length()`、`length_2d()`、`distance(other)`、`dot(other)`、`cross(other)`、`normalized()`。

运算：`+`、`-`、`* number`。

### `color`

```lua
local value = velocity.color(173, 192, 255, 220)
local faded = value:alpha(100)
```

字段：`r`、`g`、`b`、`a`，范围为 `0..255`。

## 模块控制 API

`velocity.modules` 用于操控 Velocity 现有模块。只有明确注册的模块可被访问，不提供任意指针或内存访问。

```lua
velocity.modules.exists(name)                 -- boolean
velocity.modules.get(name)                    -- boolean 或 nil
velocity.modules.state(name)                  -- 只读状态快照或 nil
velocity.modules.set(name, enabled)           -- boolean，是否成功
velocity.modules.toggle(name)                 -- 新状态或 nil
velocity.modules.list([prefix])               -- string[]
velocity.modules.bind(name)                   -- table 或 nil
velocity.modules.set_bind(name, key, mode)    -- boolean
velocity.modules.clear_bind(name)             -- boolean
```

绑定表格式：

```lua
{
    key = 86,
    mode = "hold on",
    active = false
}
```

`set_bind` 支持的模式：`toggle`、`hold_on`、`hold_off`。`key` 是 Windows 虚拟键码，`0` 表示无按键。带 `hold_on` 或 `hold_off` 绑定的模块，其开关值仍可能在每帧输入处理时被绑定状态覆盖。

```lua
velocity.modules.set("movement.bhop", true)
velocity.modules.toggle("camera.thirdperson")
velocity.modules.set_bind("combat.quickpeek", string.byte("V"), "hold_on")
```

`velocity.features.get/set/list` 保留兼容，但新脚本应使用 `velocity.modules`。

### 只读模块状态

`velocity.modules.state(name)` 查询模块当前的运行时内部状态，而 `modules.get(name)` 只查询模块开关。成功返回一个新的只读 Lua 表；未知状态名返回 `nil`。每次调用都是独立的时点快照，不会在后续 tick 自动刷新。

```lua
local rage = velocity.modules.state("ragebot")
if rage and rage.has_target then
    print(rage.target_index, rage.target_damage, rage.target_position)
end

local ok, err = pcall(function()
    rage.target_index = 1 -- 失败：状态表禁止写入
end)
```

只读边界如下：

- 表字段通过只读代理暴露，新增或修改字段会产生 Lua 错误。
- 快照不包含可调用的内部对象、lagcomp 记录或可写裸指针。
- `vec3` 等 userdata 是复制值；修改复制值不会修改模块内部状态。
- 状态字段只表示最近一次对应模块处理留下的值。应在每个需要读取的 tick/帧重新调用 `state()`。
- `enabled` 是配置开关，不一定等同于本 tick 功能实际生效；例如没有合法本地玩家、武器或目标时，开关可为 true 而 `has_target`/`active` 为 false。

支持的状态名和字段：

| 状态名/别名 | 字段 |
| --- | --- |
| `ragebot`、`ragebot.enabled` | `module`, `enabled`, `has_target`, `target_index`, `target_hitbox`, `target_hitgroup`, `target_position`, `target_damage`, `target_hitchance`, `target_fov`, `firing_this_tick`, `should_stop`, `air_stop_requested`, `revolver_cocking`, `doubletap_state`, `doubletap_progress`, `doubletap_claimed_tick`, `doubletap_last_fire_tick` |
| `legitbot`、`legitbot.enabled` | `module`, `enabled`, `has_target`, `target_index`, `target_hitbox`, `target_hitgroup`, `target_position`, `target_damage`, `target_hitchance`, `target_fov` |
| `antiaim`、`antiaim.enabled` | `module`, `enabled`, `active`, `desync_choking`, `modified_angles`, `visual_angles`, `has_visual_angles`, `final_ground_movement` |
| `combat.quickpeek`、`quickpeek` | `module`, `enabled`, `active`, `retracking` |
| `movement` | `module`, `jumpbug_active`, `jumpbug_landing_fraction`, `edgebug_active`, `test_strafer_active`, `test_strafer_handled` |
| 已注册的 `movement.*` 模块键 | 与 `movement` 相同，并在该键是布尔模块时包含 `enabled` |
| `player.local`、`local` | `module`, `valid`, `alive`, `team`, `controller`, `pawn` |

`target_hitchance` 的内部表示是 `0.0..1.0` 比例；需要百分比时乘以 100。无有效目标时目标字段保留默认值，例如索引/命中箱为 `-1`、数值为 `0`、位置为零向量。`doubletap_state` 为 `"uncharged"`、`"charging"` 或 `"ready"`。`controller` 和 `pawn` 仅作为只读诊断标识值返回，Lua API 不提供通过它们进行内存访问的函数。

## 配置控制 API

`velocity.config` 可读写已注册的模块开关、整数、浮点数、颜色和字符串。

```lua
velocity.config.exists(name)                  -- boolean
velocity.config.type(name)                    -- "boolean"/"integer"/"number"/"color"/"string" 或 nil
velocity.config.get(name)                     -- 值或 nil
velocity.config.set(name, value)              -- boolean，是否成功
velocity.config.list([prefix])                -- string[]
```

类型必须匹配。模块布尔值通过 `config.set` 修改时与 `modules.set` 行为相同，会同步 toggle 绑定状态并处理互斥模块。

```lua
velocity.config.set("ragebot.sniper.hitchance", 75)
velocity.config.set("ragebot.sniper.pointscale", 82.0)
velocity.config.set("ragebot.sniper.hittable_region_direct_color",
    velocity.color(70, 225, 145, 110))

local current = velocity.config.get("camera.fov")
velocity.log.info("current fov: " .. tostring(current))
```

`config.list()` 和 `modules.list()` 是当前构建中可用键的权威列表。可传前缀减少输出：

```lua
local rage_keys = velocity.config.list("ragebot.sniper.")
local movement_modules = velocity.modules.list("movement.")
```

## 已暴露的主要模块

### Combat

```text
ragebot.enabled
ragebot.vac_bypass
legitbot.enabled
antiaim.enabled
antiaim.vac_bypass
antiaim.auto_yaw_adjust
antiaim.manual_left
antiaim.manual_right
antiaim.hide_shots
antiaim.avoid_backstab
antiaim.direction_indicator
antiaim.direction_indicator_glow
combat.quickpeek
combat.duckpeek
combat.lagcomp_extrapolation
combat.zeusbot
combat.zeusbot_drop_after
combat.knifebot
combat.auto_revolver
combat.auto_scope
combat.penetration_crosshair
```

Rage 和 Legit 的武器组名称：`pistol`、`smg`、`rifle`、`shotgun`、`sniper`、`lmg`。

每个 Rage 组暴露以下模块：

```text
ragebot.<group>.silent
ragebot.<group>.no_spread
ragebot.<group>.doubletap
ragebot.<group>.body_aim
ragebot.<group>.force_shot
ragebot.<group>.force_shot_air
ragebot.<group>.min_damage_override
ragebot.<group>.hitchance_override
ragebot.<group>.dynamic_pointscale
ragebot.<group>.debug_multipoints
ragebot.<group>.hittable_regions
```

每个 Legit 组暴露以下模块：

```text
legitbot.<group>.aimbot
legitbot.<group>.rcs
legitbot.<group>.standalone_rcs
legitbot.<group>.triggerbot
legitbot.<group>.trigger_head_only
legitbot.<group>.autowall
legitbot.<group>.visualize_fov
```

### Movement

```text
movement.bhop
movement.airstrafe
movement.airstrafe_fully_directional
movement.jumpbug
movement.fastladder
movement.edgejump
movement.edgestop
movement.edgebug
movement.edgebug_include_jump_steps
movement.slowwalk
movement.test_strafer
movement.strafe_fix
movement.velocity_debug
```

### ESP、Chams 与 Glow

```text
esp.enemy.enabled
esp.enemy.box
esp.enemy.box_fill
esp.enemy.box_outline
esp.enemy.skeleton
esp.enemy.health
esp.enemy.ammo
esp.enemy.flags
esp.enemy.name
esp.enemy.weapon
esp.enemy.oof_arrow
esp.team.<same fields>
chams.enemy
chams.team
chams.local
chams.backtrack
chams.onshot
glow.enemy
glow.team
glow.local
esp.items
chams.items
glow.items
esp.projectiles
esp.bomb_timer
esp.spectator_list
```

### Camera、HUD、Misc、World

```text
camera.custom_fov
camera.scoped_fov_override
camera.thirdperson
camera.custom_aspect_ratio
viewmodel.enabled
hud.crosshair
hud.scope
hud.velocity_counter
hud.velocity_chart
misc.scoreboard_weapons
misc.projectile_trajectory
misc.projectile_straight_throw
misc.preserve_killfeed
misc.reveal_radar
misc.disable_game_logs
misc.dynamic_light
misc.watermark
misc.watermark_fps
misc.watermark_ping
misc.watermark_velocity
removals.crosshair
removals.scope
removals.smoke
removals.recoil
removals.decals
impacts.hit_log
impacts.miss_log
impacts.hit_sound
impacts.hit_marker
impacts.bullet_tracers
world.weather
world.weather_fog
world.weather_wetness
world.weather_wind
world.skybox
world.skybox_color_enabled
world.lighting
world.world_color_enabled
world.bloom
world.gamma
```

## 常用非布尔配置

下面列出常用键。完整列表请调用 `velocity.config.list()`。

```text
ragebot.<group>.max_fov                         number
ragebot.<group>.pointscale                      number
ragebot.<group>.hitchance                       integer
ragebot.<group>.min_damage                      integer
ragebot.<group>.min_damage_override_value       integer
ragebot.<group>.hitchance_override_value        integer
ragebot.<group>.hittable_region_duration        number
ragebot.<group>.hittable_region_direct_color    color
ragebot.<group>.hittable_region_penetrated_color color
legitbot.<group>.fov                            number
legitbot.<group>.smooth                         integer
legitbot.<group>.trigger_delay                  integer
legitbot.<group>.trigger_hitchance               integer
legitbot.<group>.min_damage                     integer
antiaim.spin_speed                              number
antiaim.jitter_range                            number
antiaim.jitter_speed                            number
combat.max_backtrack_ticks                      integer
combat.max_extrapolate_ticks                    integer
movement.edgebug_mode                           integer
movement.edgebug_passes                         integer
movement.slowwalk_speed                         number
camera.fov                                      number
camera.scoped_fov                               number
camera.thirdperson_distance                     number
camera.aspect_ratio                             number
viewmodel.offset_x/y/z                          number
viewmodel.fov                                   number
removals.flash_alpha                            number
misc.dynamic_light_radius                       number
misc.dynamic_light_color                        color
impacts.hit_log_duration                        number
impacts.hit_sound_volume                        number
impacts.hit_marker_color                        color
impacts.custom_hit_sound                        string
name_changer.name                               string
world.weather_color                             color
world.weather_fog_density                       number
world.weather_wind_strength                     number
world.skybox_color                              color
world.lighting_color                            color
world.lighting_intensity                        number
world.world_color                               color
world.bloom_value                               number
world.gamma_value                               number
```

## 实体与玩家

实体 API 返回快照，不暴露游戏对象地址。快照不会自动更新。

```lua
velocity.entity.by_index(index)                 -- entity 或 nil
velocity.entity.list(type)                      -- entity[]
velocity.entity.players()                       -- entity[]
velocity.entity.items()                         -- entity[]
velocity.entity.projectiles()                   -- entity[]
velocity.player.local()                         -- entity
velocity.player.list()                          -- entity[]
```

`entity` 字段：`index`、`type`、`class_name`、`health`、`team`、`alive`、`enemy`、`origin`、`velocity`。

## 视图、追踪、数学和时间

```lua
velocity.view.origin()                          -- vec3
velocity.view.angles()                          -- vec3
velocity.view.fov()                             -- number
velocity.view.project(world)                    -- projection

velocity.trace.line(start, finish)              -- trace_result
velocity.trace.hull(start, finish, mins, maxs)  -- trace_result
velocity.trace.sphere(start, finish, radius)     -- trace_result
velocity.trace.to_entity(start, finish, index)  -- trace_result 或 nil
velocity.trace.visible(start, finish, index)    -- boolean
velocity.trace.hitbox(start, index, hitbox)     -- trace_result 或 nil
velocity.trace.most_visible_bone(start, index)  -- bone 或 nil

velocity.math.clamp(value, min, max)
velocity.math.normalize_yaw(yaw)
velocity.math.angle_to(from, to)
velocity.math.angle_distance(from, to)

velocity.time.delta_time()
velocity.time.tick_interval()
velocity.time.current_time()
velocity.time.tick()
velocity.screen.size()
velocity.screen.center()

velocity.schedule(delay, callback)              -- task_id 或 nil
velocity.cancel_schedule(task_id)               -- boolean
```

`projection` 字段：`screen`、`valid`、`on_screen`。

`trace_result` 字段：`fraction`、`position`、`normal`、`hit_index`、`hit`。

## 绘制 API

仅在 `paint` 回调中使用：

```lua
velocity.draw.line(start, finish, color [, thickness])
velocity.draw.rect(position, size, color [, thickness])
velocity.draw.filled_rect(position, size, color)
velocity.draw.circle(center, radius, color [, thickness])
velocity.draw.filled_circle(center, radius, color)
velocity.draw.triangle(a, b, c, color)
velocity.draw.text(position, text, color)
velocity.draw.measure_text(text)                 -- vec2
velocity.draw.world_line(start, finish, color [, thickness])
```

## Command API

仅在 `create_move_pre`、`create_move` 或 `create_move_post` 命令回调中使用：

```lua
command:number()
command:tick()
command:view_angles()
command:forward_move()
command:side_move()
command:up_move()
command:has_button(name)
command:set_button(name, enabled)
command:set_movement(forward, side [, up])
command:set_view_angles(angles)
command:set_silent_angles(angles)
command:set_camera_angles(angles)
command:fire([secondary])
command:stop_firing()
```

按钮名：`attack`、`attack1`、`attack2`、`secondary_attack`、`jump`、`duck`、`use`、`reload`、`forward`、`back`、`left`、`right`、`sprint`、`zoom`。

## Lua UI API

仅在 `menu` 回调中使用。控件状态按脚本隔离并保持到卸载或重载。

```lua
velocity.ui.text(text)
velocity.ui.separator()
velocity.ui.checkbox(key, default)
velocity.ui.slider_float(key, default, min, max)
velocity.ui.slider_int(key, default, min, max)
velocity.ui.combo(key, default_index, options)
velocity.ui.color_picker(key, default_color)
velocity.ui.input_text(key, default_text [, max_length])
velocity.ui.button(key)
velocity.ui.get(key)
velocity.ui.reset(key)
```

### 调试与控制台

Lua 页提供 `lua console` 按钮，可打开独立的 `lua console` 窗口查看脚本输出、警告和错误。窗口支持滚动、拖动和缩放。

Lua 页还提供以下操作：

- `clear lua console`：清空当前控制台缓冲。
- `export console log`：将控制台导出到 `C:\velocity-pro\lua-console.log`。

控制台最多保留最近 1000 条消息。每条消息包含脚本文件名和级别。

```lua
print("普通输出", 123)
Debug("调试信息", velocity.version)
velocity.debug("等价的调试输出")

velocity.log.info("信息")
velocity.log.warn("警告")
velocity.log.error("错误")
```

`print(...)`、`Debug(...)` 和 `velocity.debug(...)` 都会写入 `INFO` 级别；`velocity.log.warn(...)` 写入 `WARN`；`velocity.log.error(...)` 写入 `ERROR`。这些函数支持多个参数，参数之间以制表符分隔。

## Lua 配置持久化

Lua 脚本状态会随主配置一起保存，不需要脚本自行写文件。配置保存时会记录：

- 脚本文件名及其当前是否已加载；
- `velocity.ui.checkbox`、`slider_int`、`slider_float`、`combo`、`color_picker` 和 `input_text` 控件的当前值。

加载主配置时，系统先恢复该脚本已保存的 UI 值，再按配置中的加载状态创建 Lua VM 并执行脚本和 `on_load()`。因此脚本首次调用对应的 `velocity.ui.*` 控件时就能读取恢复值，控件提供的默认值只在配置中没有该键时生效。脚本文件不存在、加载失败或脚本曾触发运行时错误时，会跳过该脚本，不影响其他配置字段。

配置序列化、配置恢复、脚本加载/卸载和 Lua 回调使用同一套串行化保护。保存配置时获得的是完整一致的脚本加载状态和 UI 快照，不会与正在执行的 `menu` 或命令回调同时修改同一个 Lua 状态。

配置保存覆盖菜单中的 `save`、导入后自动保存和分享码导出；旧配置没有 Lua 字段时按“无脚本自动加载”处理。Lua 控件键只在同一脚本文件内有意义，重命名脚本文件会被视为新脚本。

脚本回调发生错误后会记录到 Lua console，并停用该脚本后续回调，避免错误脚本在每个 tick 重复执行。修复脚本后可在 Lua 页面执行 refresh/reload，再重新加载脚本。

## 模块控制示例

```lua
local group = "sniper"

function on_menu()
    local rage = velocity.modules.get("ragebot.enabled") or false
    local requested = velocity.ui.checkbox("Lua ragebot control", rage)
    if requested ~= rage then
        velocity.modules.set("ragebot.enabled", requested)
    end

    local hc_key = "ragebot." .. group .. ".hitchance"
    local hitchance = velocity.config.get(hc_key) or 80
    local changed = velocity.ui.slider_int("Sniper hitchance", hitchance, 0, 100)
    if changed ~= hitchance then
        velocity.config.set(hc_key, changed)
    end

    local color_key = "ragebot." .. group .. ".hittable_region_direct_color"
    local current_color = velocity.config.get(color_key) or velocity.color(75, 220, 145, 95)
    local new_color = velocity.ui.color_picker("Hitbox color", current_color)
    velocity.config.set(color_key, new_color)
end
```

完整示例文件：[examples/module_control.lua](examples/module_control.lua)。

## 边界与注意事项

- Lua API 不提供任意内存读写、裸指针、hook 注册或系统调用。
- `modules.set` 和 `config.set` 修改的是当前运行配置，不是脚本私有副本。
- 多个脚本修改同一个键时，最后一次写入生效。
- 对带 hold 绑定的模块直接设置布尔值，下一次绑定处理可能覆盖该值。
- 脚本卸载不会恢复模块原值；需要恢复时请由脚本保存旧值并在自己的逻辑中恢复。
- 不要在每帧回调中无条件重复写入大量配置；只在值变化时调用 `set`。
- 脚本错误会显示在 Lua 菜单和控制台中。修复后使用 `reload all`。

## 日志与脚本管理

```lua
velocity.log.info(message)
velocity.log.warn(message)
velocity.reload_all()
```
