require("/scripts/core/core.lua")
require("/scripts/core/math.lua")
require("/scripts/system.lua")
require("/scripts/helper_nodes.lua")
require("/scripts/app/clover_task.lua")
require("/scripts/app/clover_util.lua")
require("/scripts/app/clover_event.lua")
require("/scripts/app/clover_scene.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/tween/tween_ease.lua")

local var_0_0 = class()

CloverBG = {
	Init = function(arg_1_0, arg_1_1)
		local var_1_0 = CloverBG

		var_1_0.scene = CloverScene.AddResource("bg_scene", arg_1_0)
		var_1_0.is_loaded = false
		var_1_0.camera = arg_1_1
		var_1_0.scroll_bg_tbl = {}
		var_1_0.current_bg = "default"
		var_1_0.next_bg = "default"

		CloverTask.Register("bg", CloverBG.Update)

		local var_1_1 = CloverState.new()

		var_1_1:add("loading", var_1_0.StateLoading)
		var_1_1:add("usual", var_1_0.StateUsual)
		var_1_1:add("switch", var_1_0.StateSwitch)
		var_1_1:add("resumeon", var_1_0.StateResumeOn)
		var_1_1:change("loading")

		var_1_0.state = var_1_1
		var_1_0.event = CloverEvent.new()

		system.waitBootFade("bg", true)

		var_1_0.fade_seconds = 0.5
		var_1_0.mosaic = nil
		var_1_0.resume_on = false
		var_1_0.alpha_tween = nil
		var_1_0.down_tween = nil
		var_1_0.down_start_y = 0
		var_1_0.up_start_y = 0
		var_1_0.resumeon_soon = false

		if system.is_roll_back_cancel then
			var_1_0.resumeon_soon = true
		end
	end,
	Enable = function()
		CloverTask.Awake("bg")

		if CloverBG.is_loaded then
			CloverBG.scene:GetNode():enable()
		end
	end,
	Disable = function()
		CloverTask.Sleep("bg")

		if CloverBG.is_loaded then
			CloverBG.scene:GetNode():disable()
		end
	end,
	SetConfig = function(arg_4_0, arg_4_1)
		CloverBG.scroll_bg_tbl.demo:SetConfig(CloverConst.BG[arg_4_1])
	end,
	ResumeOn = function()
		CloverBG.resume_on = true
	end,
	ResumeOff = function()
		CloverBG.resume_on = false

		local var_6_0 = CloverBG

		if CloverTask.IsSleep("bg") then
			CloverBG.Enable()
		end
	end,
	Update = function(arg_7_0)
		local var_7_0 = CloverBG

		var_7_0.state:update(arg_7_0)

		if var_7_0.scroll_bg_tbl[var_7_0.current_bg] then
			var_7_0.scroll_bg_tbl[var_7_0.current_bg]:update(arg_7_0)
		end

		if var_7_0.mosaic then
			var_7_0.mosaic:update(arg_7_0)
		end
	end,
	GetCurrent = function()
		local var_8_0 = CloverBG

		return var_8_0.scroll_bg_tbl[var_8_0.current_bg]
	end,
	ScrollLeft = function()
		if CloverBG.scroll_bg_tbl[CloverBG.current_bg] then
			CloverBG.scroll_bg_tbl[CloverBG.current_bg]:ScrollLeft()
		end
	end,
	ScrollRight = function()
		if CloverBG.scroll_bg_tbl[CloverBG.current_bg] then
			CloverBG.scroll_bg_tbl[CloverBG.current_bg]:ScrollRight()
		end
	end,
	SetScrollEnable = function(arg_11_0)
		local var_11_0 = CloverBG

		for iter_11_0, iter_11_1 in pairs(var_11_0.scroll_bg_tbl) do
			iter_11_1:SetScrollEnable(arg_11_0)
		end
	end,
	SetFadeRate = function(arg_12_0)
		local var_12_0 = CloverBG

		for iter_12_0, iter_12_1 in pairs(var_12_0.scroll_bg_tbl) do
			iter_12_1:SetFadeRate(arg_12_0)
		end
	end,
	Switch = function(arg_13_0, arg_13_1)
		local var_13_0 = CloverBG

		var_13_0.next_bg = arg_13_0
		var_13_0.fade_seconds = arg_13_1

		if arg_13_1 == 0 then
			var_13_0.current_bg = var_13_0.next_bg

			if var_13_0.is_loaded then
				CloverBG.UpdateVisible()
			end
		end
	end,
	UpdateVisible = function()
		local var_14_0 = CloverBG

		assert(var_14_0.scene:IsReady())

		for iter_14_0, iter_14_1 in pairs(var_14_0.scroll_bg_tbl) do
			iter_14_1:SetVisible(false)
		end

		var_14_0.GetCurrent():SetVisible(true)

		local var_14_1, var_14_2, var_14_3 = var_14_0.GetCurrent():GetBackColor()

		if var_14_0.current_bg == "demo" then
			local var_14_4 = CloverConst.BG.DEMO_BG_DARK_RATE

			var_14_1 = var_14_1 * var_14_4
			var_14_2 = var_14_2 * var_14_4
			var_14_3 = var_14_3 * var_14_4
		end

		var_14_0.camera:setClearColor(var_14_1, var_14_2, var_14_3, 1)
	end,
	StateLoading = function(arg_15_0)
		local var_15_0 = CloverBG
		local var_15_1 = var_15_0.event

		if arg_15_0:is_first() then
			var_15_1:reset()
		end

		local var_15_2 = {
			function()
				var_15_0.scene:Link()
				var_15_0.scene:AddNode()
				var_15_0.scene:SetVisible(false)
				var_15_1:next_index()
			end,
			function()
				if var_15_0.scene:IsReady() then
					var_15_0.scene:SetVisible(true)

					local var_17_0 = CloverConst.BG.default

					var_15_0.scroll_bg_tbl.default = CloverScrollBG.new(var_15_0.scene:GetNode(), "default_bg", var_17_0)

					local var_17_1 = CloverConst.BG.demo

					var_15_0.scroll_bg_tbl.demo = CloverScrollBG.new(var_15_0.scene:GetNode(), "demo_bg", var_17_1)

					if system.is_nes() then
						var_15_0.scroll_bg_tbl.default:SetBackColor(0, 0, 0)
					else
						var_15_0.scroll_bg_tbl.default:SetBackColor(0.4, 0.43, 0.4)
					end

					var_15_0.scroll_bg_tbl.demo:SetBackColor(0, 0.3843137254901961, 0.7411764705882353)

					local var_17_2 = var_15_0.scene:GetNode()

					var_15_0.mosaic = var_0_0.new(var_17_2.camera_node, var_17_2.render_node)

					for iter_17_0, iter_17_1 in pairs(var_15_0.scroll_bg_tbl) do
						var_15_0.mosaic:AddTargetNode(iter_17_1:GetRoot())
					end

					CloverBG.UpdateVisible()
					system.waitBootFade("bg", false)

					if var_15_0.resume_on then
						if var_15_0.resumeon_soon then
							local var_17_3 = var_15_0.scene:GetNode()

							var_17_3.resume_hide:setAlpha(0)
							var_17_3:disable()
						end

						arg_15_0:set_next("resumeon")
					else
						arg_15_0:set_next("usual")
					end

					var_15_0.is_loaded = true

					var_15_1:next_index()
				end
			end
		}

		var_15_1:update(var_15_2, arg_15_0.dt)
	end,
	StateUsual = function(arg_18_0)
		local var_18_0 = CloverBG

		if var_18_0.resume_on then
			arg_18_0:set_next("resumeon")
		end

		if var_18_0.current_bg ~= var_18_0.next_bg then
			arg_18_0:set_next("switch")
		end
	end,
	StateSwitch = function(arg_19_0)
		local var_19_0 = CloverBG
		local var_19_1 = var_19_0.event

		if arg_19_0:is_first() then
			var_19_1:reset()

			arg_19_0.local_value.fade_passed_time = 0

			var_19_0.mosaic:Enable()
			var_19_0.SetScrollEnable(false)
		end

		local function var_19_2(arg_20_0)
			local var_20_0 = arg_19_0.local_value.fade_passed_time + arg_19_0.dt

			if arg_20_0 <= var_20_0 then
				var_20_0 = arg_20_0
			end

			arg_19_0.local_value.fade_passed_time = var_20_0

			return var_20_0
		end

		local function var_19_3()
			arg_19_0.local_value.fade_passed_time = 0
		end

		local var_19_4 = {
			function()
				local var_22_0 = var_19_2(var_19_0.fade_seconds)
				local var_22_1 = false

				if var_22_0 == var_19_0.fade_seconds then
					local var_22_2 = var_19_0.GetCurrent().scroll_dir

					var_19_0.current_bg = var_19_0.next_bg

					var_19_0.UpdateVisible()

					var_19_0:GetCurrent().scroll_dir = var_22_2
					var_22_1 = true
				end

				local var_22_3 = var_22_0 / var_19_0.fade_seconds

				var_19_0.SetFadeRate(var_22_3)
				var_19_0.mosaic:SetRate(var_22_3)

				if var_22_1 then
					var_19_3()
					var_19_1:next_index()
				end
			end,
			function()
				var_19_1:wait(CloverConst.BG.SWITCH_BG_WAIT_SECONDS)
				var_19_1:next_index()
			end,
			function()
				local var_24_0 = var_19_2(var_19_0.fade_seconds)
				local var_24_1 = 1 - var_24_0 / var_19_0.fade_seconds

				var_19_0.SetFadeRate(var_24_1)
				var_19_0.mosaic:SetRate(var_24_1)

				if var_24_0 == var_19_0.fade_seconds then
					arg_19_0:set_next("usual")
					var_19_0.mosaic:Disable()
					var_19_0.SetScrollEnable(true)
					var_19_1:next_index()
				end
			end
		}

		var_19_1:update(var_19_4, arg_19_0.dt)
	end,
	StateResumeOn = function(arg_25_0)
		local var_25_0 = CloverBG

		local function var_25_1()
			-- Freeze the wallpaper behind the Suspend Point List, matching hardware. An idle framebuffer
			-- capture of the device resume menu shows interframe motion ~0 across every neon region
			-- (including the pure-neon strip above the menubar): the device stops the scroll and holds
			-- the last frame while the list is open. The original code called CloverBG.Disable() here,
			-- which both sleeps the "bg" task (the freeze) and disables the bg scene node. The device
			-- renders the wallpaper through a persistent background camera that keeps drawing the last
			-- frame after the node is disabled, so it reads as visible-but-frozen; the web renders the bg
			-- as an ordinary scene node, so disabling it blanks the neon to black (the "background
			-- disappears" bug). Sleep the task without disabling the node to reproduce the device's
			-- visible-but-frozen result. ResumeOff re-Enables (Awake) the task on close, so scroll resumes.
			if CloverBG.resume_on then
				CloverTask.Sleep("bg")
			end
		end

		if arg_25_0:is_first() then
			local var_25_2 = CloverConst.BG.RESUME_CHANGE_ALPHA_SECONDS

			if var_25_0.current_bg == "default" then
				local var_25_3 = var_25_0.scene:GetNode()

				tween_stop(var_25_0.alpha_tween)

				if var_25_0.resumeon_soon then
					var_25_2 = 0
					var_25_0.resumeon_soon = false
				end

				var_25_0.alpha_tween = Tween:sequence(Tween:alphaTo(var_25_3.resume_hide, var_25_2, 0), Tween:callback(var_25_1)):start()
			else
				local var_25_4 = var_25_0.scene:GetNode()
				local var_25_5 = var_25_4.resume_down
				local var_25_6 = var_25_4.resume_up
				local var_25_7 = var_25_4.resume_hide_demo

				var_25_0.down_start_y = var_25_5:getLocalPositionY()
				var_25_0.up_start_y = var_25_6:getLocalPositionY()

				tween_stop(var_25_0.down_tween)

				var_25_0.down_tween = Tween:parallel(Tween:sequence(Tween:wait(0.05), Tween:moveBy(var_25_5, 0.125, 0, -50), Tween:worldNodeEnabledTo(var_25_5, 0, false)), Tween:sequence(Tween:alphaTo(var_25_7, var_25_2, 0), Tween:worldNodeEnabledTo(var_25_6, 0, false), Tween:callback(var_25_1))):start()
			end
		end

		if var_25_0.resume_on == false then
			if var_25_0.current_bg == "default" then
				local var_25_8 = var_25_0.scene:GetNode()

				tween_stop(var_25_0.alpha_tween)

				var_25_0.alpha_tween = Tween:sequence(Tween:alphaTo(var_25_8.resume_hide, CloverConst.BG.RESUME_CHANGE_ALPHA_SECONDS, 1)):start()
			else
				local var_25_9 = var_25_0.scene:GetNode()
				local var_25_10 = var_25_9.resume_down
				local var_25_11 = var_25_9.resume_up
				local var_25_12 = var_25_9.resume_hide_demo

				tween_stop(var_25_0.down_tween)

				var_25_0.down_tween = Tween:parallel(Tween:sequence(Tween:worldNodeEnabledTo(var_25_10, 0, true), Tween:moveTo(var_25_10, 0, var_25_10:getLocalPositionX(), var_25_0.down_start_y)), Tween:sequence(Tween:worldNodeEnabledTo(var_25_11, 0, true), Tween:alphaTo(var_25_12, CloverConst.BG.RESUME_CHANGE_ALPHA_SECONDS, 1))):start()
			end

			arg_25_0:set_next("usual")
		end
	end
}
CloverScrollBG = class()

function CloverScrollBG.new(arg_27_0, arg_27_1, arg_27_2)
	local var_27_0 = new(CloverScrollBG)

	var_27_0:init(arg_27_0, arg_27_1, arg_27_2)

	return var_27_0
end

function CloverScrollBG.init(arg_28_0, arg_28_1, arg_28_2, arg_28_3)
	arg_28_0.root = arg_28_1:getChildByName(arg_28_2)
	arg_28_0.anim = arg_28_0.root:getChildByName("RootPane"):getComponent(AnimatorComponent)

	arg_28_0.anim:stop()

	arg_28_0.config_tbl = arg_28_3
	arg_28_0.cursor_scroll_time = 0
	arg_28_0.cursor_scroll_speed = 0
	arg_28_0.scroll_dir = 1
	arg_28_0.current_scroll_speed = arg_28_0.config_tbl.DEFAULT_SCROLL_SPEED
	arg_28_0.fade_rate = 0
	arg_28_0.fade_dirty = false
	arg_28_0.is_scroll_enable = true
	arg_28_0.back_r = 0
	arg_28_0.back_g = 0
	arg_28_0.back_b = 0
end

function CloverScrollBG.update(arg_29_0, arg_29_1)
	arg_29_0:update_scroll(arg_29_1)
	arg_29_0:update_fade()
end

function CloverScrollBG.GetRoot(arg_30_0)
	return arg_30_0.root
end

function CloverScrollBG.SetScrollEnable(arg_31_0, arg_31_1)
	arg_31_0.is_scroll_enable = arg_31_1
end

function CloverScrollBG.SetVisible(arg_32_0, arg_32_1)
	arg_32_0.root:setEnabled(arg_32_1)
end

function CloverScrollBG.SetBackColor(arg_33_0, arg_33_1, arg_33_2, arg_33_3)
	arg_33_0.back_r = arg_33_1
	arg_33_0.back_g = arg_33_2
	arg_33_0.back_b = arg_33_3
end

function CloverScrollBG.GetBackColor(arg_34_0)
	return arg_34_0.back_r, arg_34_0.back_g, arg_34_0.back_b
end

function CloverScrollBG.SetFadeRate(arg_35_0, arg_35_1)
	arg_35_1 = Math.clamp(arg_35_1, 0, 1)
	arg_35_0.fade_rate = arg_35_1
	arg_35_0.fade_dirty = true
end

function CloverScrollBG.SetConfig(arg_36_0, arg_36_1)
	arg_36_0.config_tbl = arg_36_1
end

function CloverScrollBG.ScrollLeft(arg_37_0)
	if arg_37_0.cursor_scroll_time == 0 then
		arg_37_0.cursor_scroll_time = arg_37_0.config_tbl.SCROLL_CURSOR_SECONDS
		arg_37_0.cursor_scroll_speed = -arg_37_0.config_tbl.SCROLL_CURSOR_SPEED
	else
		arg_37_0.cursor_scroll_time = arg_37_0.config_tbl.SCROLL_CURSOR_SECONDS - arg_37_0.config_tbl.SCROLL_CURSOR_DELAY
		arg_37_0.cursor_scroll_speed = arg_37_0.cursor_scroll_speed - arg_37_0.config_tbl.SCROLL_CURSOR_ACCEL

		if arg_37_0.cursor_scroll_speed < -arg_37_0.config_tbl.SCROLL_CURSOR_SPEED_MAX then
			arg_37_0.cursor_scroll_speed = -arg_37_0.config_tbl.SCROLL_CURSOR_SPEED_MAX
		end
	end

	arg_37_0.scroll_dir = -1
end

function CloverScrollBG.ScrollRight(arg_38_0)
	if arg_38_0.cursor_scroll_time == 0 then
		arg_38_0.cursor_scroll_time = arg_38_0.config_tbl.SCROLL_CURSOR_SECONDS
		arg_38_0.cursor_scroll_speed = arg_38_0.config_tbl.SCROLL_CURSOR_SPEED
	else
		arg_38_0.cursor_scroll_time = arg_38_0.config_tbl.SCROLL_CURSOR_SECONDS - arg_38_0.config_tbl.SCROLL_CURSOR_DELAY
		arg_38_0.cursor_scroll_speed = arg_38_0.cursor_scroll_speed + arg_38_0.config_tbl.SCROLL_CURSOR_ACCEL

		if arg_38_0.cursor_scroll_speed > arg_38_0.config_tbl.SCROLL_CURSOR_SPEED_MAX then
			arg_38_0.cursor_scroll_speed = arg_38_0.config_tbl.SCROLL_CURSOR_SPEED_MAX
		end
	end

	arg_38_0.scroll_dir = 1
end

function CloverScrollBG.update_scroll(arg_39_0, arg_39_1)
	if arg_39_0.is_scroll_enable == false then
		return
	end

	local var_39_0 = arg_39_0.scroll_dir * arg_39_0.config_tbl.DEFAULT_SCROLL_SPEED

	arg_39_0.cursor_scroll_time = arg_39_0.cursor_scroll_time - arg_39_1

	if arg_39_0.cursor_scroll_time < 0 then
		arg_39_0.cursor_scroll_time = 0
	end

	if arg_39_0.cursor_scroll_time > 0 and arg_39_0.cursor_scroll_time < arg_39_0.config_tbl.SCROLL_CURSOR_SECONDS - arg_39_0.config_tbl.SCROLL_CURSOR_DELAY then
		var_39_0 = var_39_0 + arg_39_0.cursor_scroll_speed
	else
		if arg_39_0.cursor_scroll_speed > 0 then
			arg_39_0.cursor_scroll_speed = arg_39_0.cursor_scroll_speed - arg_39_0.config_tbl.SCROLL_CURSOR_DECEL

			if arg_39_0.cursor_scroll_speed < 0 then
				arg_39_0.cursor_scroll_speed = 0
			end
		else
			arg_39_0.cursor_scroll_speed = arg_39_0.cursor_scroll_speed + arg_39_0.config_tbl.SCROLL_CURSOR_DECEL

			if arg_39_0.cursor_scroll_speed > 0 then
				arg_39_0.cursor_scroll_speed = 0
			end
		end

		var_39_0 = var_39_0 + arg_39_0.cursor_scroll_speed
	end

	if arg_39_0.config_tbl.SCROLL_SMOOTH then
		arg_39_0.current_scroll_speed = arg_39_0.current_scroll_speed + (var_39_0 - arg_39_0.current_scroll_speed) * 0.125
	else
		arg_39_0.current_scroll_speed = var_39_0
	end

	arg_39_0.anim:advanceCurrentTime(arg_39_1 * arg_39_0.current_scroll_speed)
end

function CloverScrollBG.update_fade(arg_40_0)
	if arg_40_0.fade_dirty == false then
		return
	end

	local var_40_0 = arg_40_0.fade_rate
	local var_40_1 = 1 - Ease.inQuad(var_40_0, 0, 1, 1)

	arg_40_0.root:setColor(var_40_1, var_40_1, var_40_1, 1)

	arg_40_0.fade_dirty = false
end

local var_0_1 = {}

function var_0_0.new(arg_41_0, arg_41_1)
	local var_41_0 = new(var_0_0)

	var_41_0:init(arg_41_0, arg_41_1)

	return var_41_0
end

function var_0_0.init(arg_42_0, arg_42_1, arg_42_2)
	arg_42_0.camera_node = arg_42_1
	arg_42_0.render_node = arg_42_2
	arg_42_0.camera_component = arg_42_0.camera_node:getComponent(CameraComponent)
	arg_42_0.render_component = arg_42_0.render_node:getComponent(TextureComponent)

	arg_42_0.camera_node:disable()
	arg_42_0.render_node:disable()

	arg_42_0.default_category = 0
	arg_42_0.mosaic_category = 1
	arg_42_0.render_default_scale = 0.25
	arg_42_0.mosaic_targets = {}
	arg_42_0.is_enable_mosaic = false
	arg_42_0.mosaic_rate = 0

	local var_42_0, var_42_1 = arg_42_0.render_component:getSize()

	arg_42_0.base_texture_w = var_42_0
	arg_42_0.base_texture_h = var_42_1
end

function var_0_0.update(arg_43_0, arg_43_1)
	if not arg_43_0.is_enable_mosaic then
		return
	end

	local var_43_0 = 1
	local var_43_1 = Ease.linear(arg_43_0.mosaic_rate, 1, CloverConst.BG.MOSAIC_MAX_SCALE, 1)
	local var_43_2 = 1 / var_43_1 * arg_43_0.render_default_scale
	local var_43_3 = arg_43_0.base_texture_w * var_43_1
	local var_43_4 = arg_43_0.base_texture_h * var_43_1

	arg_43_0.render_component:setSize(var_43_3, var_43_4)

	for iter_43_0, iter_43_1 in ipairs(arg_43_0.mosaic_targets) do
		iter_43_1:setLocalScale(var_43_2, var_43_2)
	end
end

function var_0_0.AddTargetNode(arg_44_0, arg_44_1)
	table.insert(arg_44_0.mosaic_targets, arg_44_1)

	if arg_44_0.is_enable_mosaic then
		var_0_1.SetNodeCategory(arg_44_1, arg_44_0.mosaic_category)
	else
		var_0_1.SetNodeCategory(arg_44_1, arg_44_0.default_category)
	end
end

function var_0_0.SetRate(arg_45_0, arg_45_1)
	arg_45_0.mosaic_rate = arg_45_1
end

function var_0_0.Enable(arg_46_0)
	if arg_46_0.is_enable_mosaic then
		return
	end

	for iter_46_0, iter_46_1 in ipairs(arg_46_0.mosaic_targets) do
		var_0_1.SetNodeCategory(iter_46_1, arg_46_0.mosaic_category)
	end

	arg_46_0.camera_node:enable()
	arg_46_0.render_node:enable()
	arg_46_0:update(0)

	arg_46_0.is_enable_mosaic = true
end

function var_0_0.Disable(arg_47_0)
	if not arg_47_0.is_enable_mosaic then
		return
	end

	for iter_47_0, iter_47_1 in ipairs(arg_47_0.mosaic_targets) do
		var_0_1.SetNodeCategory(iter_47_1, arg_47_0.default_category)
	end

	arg_47_0.camera_node:disable()
	arg_47_0.render_node:disable()

	for iter_47_2, iter_47_3 in ipairs(arg_47_0.mosaic_targets) do
		iter_47_3:setLocalScale(1, 1)
	end

	arg_47_0.is_enable_mosaic = false
end

function var_0_1.SetNodeCategory(arg_48_0, arg_48_1)
	local function var_48_0(arg_49_0)
		arg_49_0:setCategory(arg_48_1)

		for iter_49_0 in iterate_children(arg_49_0) do
			var_48_0(iter_49_0)
		end
	end

	var_48_0(arg_48_0)
end
