require("/scripts/core/core.lua")
require("/scripts/tween/tween.lua")
require("/scripts/tween/tween_ease.lua")
require("/scripts/helper_tween.lua")
require("/scripts/helper_nodes.lua")
require("/scripts/app/clover_const.lua")

sys_fade = class(WorldNode)

local var_0_0 = {}

function sys_fade.start(arg_1_0)
	arg_1_0.fadeInstance = {
		black = var_0_0.CreateInstance("black", arg_1_0.node_black),
		white = var_0_0.CreateInstance("white", arg_1_0.node_black)
	}

	if arg_1_0.node_blind then
		arg_1_0.fadeInstance.blind = var_0_0.CreateInstance("blind", arg_1_0.node_blind)
	end

	arg_1_0.fade_type = CloverConst.Fade.NORMAL_FADE_TYPE

	arg_1_0:setType(arg_1_0.fade_type)
end

function sys_fade.stop(arg_2_0)
	print("sys_fade:stop()")
end

function sys_fade.update(arg_3_0, arg_3_1)
	arg_3_0:current():update(arg_3_1)
end

function sys_fade.current(arg_4_0)
	return arg_4_0.fadeInstance[arg_4_0.fade_type]
end

function sys_fade.setType(arg_5_0, arg_5_1)
	local var_5_0 = arg_5_0:current()

	arg_5_0.fade_type = arg_5_1

	var_5_0:disable()
	arg_5_0:current():disable()
end

function sys_fade.isRunning(arg_6_0)
	return arg_6_0:isEnabled() and arg_6_0:current():isRunning()
end

function sys_fade.isInEnd(arg_7_0)
	return arg_7_0:current():isInEnd()
end

function sys_fade.isOutEnd(arg_8_0)
	return arg_8_0:current():isOutEnd()
end

function sys_fade.In(arg_9_0, arg_9_1, arg_9_2, arg_9_3)
	arg_9_0:current():In(arg_9_1, arg_9_2, arg_9_3)
end

function sys_fade.Out(arg_10_0, arg_10_1, arg_10_2, arg_10_3)
	arg_10_0:current():Out(arg_10_1, arg_10_2, arg_10_3)
end

local var_0_1 = class()

function var_0_1.init(arg_11_0, arg_11_1)
	arg_11_0.node = arg_11_1
	arg_11_0.is_enable = false
	arg_11_0.is_running = false
	arg_11_0.is_out_started = false
	arg_11_0.is_in_started = false

	arg_11_0:disable()
end

function var_0_1.enable(arg_12_0)
	arg_12_0.node:enable()

	arg_12_0.is_enable = true
end

function var_0_1.disable(arg_13_0)
	arg_13_0.node:disable()

	arg_13_0.is_enable = false
end

function var_0_1.isEnable(arg_14_0)
	return arg_14_0.is_enable
end

function var_0_1.isRunning(arg_15_0)
	return arg_15_0.is_running
end

function var_0_1.isInEnd(arg_16_0)
	return arg_16_0.is_running == false and arg_16_0.is_in_started
end

function var_0_1.isOutEnd(arg_17_0)
	return arg_17_0.is_running == false and arg_17_0.is_out_started
end

function var_0_1.update(arg_18_0, arg_18_1)
	if arg_18_0.is_running == false then
		return
	end

	arg_18_0:doUpdate(arg_18_1)
end

function var_0_1.doUpdate(arg_19_0, arg_19_1)
	return
end

function var_0_1.In(arg_20_0, arg_20_1, arg_20_2, arg_20_3)
	arg_20_0.is_out_started = false
	arg_20_0.is_in_started = true

	arg_20_0:doIn(arg_20_1, arg_20_2, arg_20_3)
end

function var_0_1.doIn(arg_21_0, arg_21_1, arg_21_2, arg_21_3)
	return
end

function var_0_1.Out(arg_22_0, arg_22_1, arg_22_2, arg_22_3)
	arg_22_0.is_out_started = true
	arg_22_0.is_in_started = false

	arg_22_0:doOut(arg_22_1, arg_22_2, arg_22_3)
end

function var_0_1.doOut(arg_23_0, arg_23_1, arg_23_2, arg_23_3)
	return
end

local var_0_2 = class(var_0_1)

function var_0_2.init(arg_24_0, arg_24_1)
	var_0_1.init(arg_24_0, arg_24_1)

	arg_24_0.tween = nil
	arg_24_0.component = arg_24_1:getComponent(VisualComponent)

	if not arg_24_0.r then
		arg_24_0.r = 0
	end

	if not arg_24_0.g then
		arg_24_0.g = 0
	end

	if not arg_24_0.b then
		arg_24_0.b = 0
	end
end

function var_0_2.setColor(arg_25_0, arg_25_1, arg_25_2, arg_25_3)
	arg_25_0.r = arg_25_1
	arg_25_0.g = arg_25_2
	arg_25_0.b = arg_25_3
end

function var_0_2.doIn(arg_26_0, arg_26_1, arg_26_2, arg_26_3)
	tween_stop(arg_26_0.tween)
	arg_26_0.component:setColor(arg_26_0.r, arg_26_0.g, arg_26_0.b, 1)

	arg_26_0.is_running = true

	local function var_26_0(arg_27_0)
		if arg_26_2 then
			arg_26_2()
		end
	end

	local function var_26_1(arg_28_0)
		if arg_26_3 then
			arg_26_3()
		end

		arg_28_0:disable()

		arg_28_0.is_running = false
	end

	arg_26_0:enable()

	arg_26_0.tween = Tween:sequence(Tween:wait(arg_26_1), Tween:callback(var_26_0, arg_26_0), Tween:alphaTo(arg_26_0.component, CloverConst.Fade.IN_DURATION, 0), Tween:callback(var_26_1, arg_26_0)):start()
end

function var_0_2.doOut(arg_29_0, arg_29_1, arg_29_2, arg_29_3)
	tween_stop(arg_29_0.tween)
	arg_29_0.component:setColor(arg_29_0.r, arg_29_0.g, arg_29_0.b, 0)

	arg_29_0.is_running = true

	local function var_29_0(arg_30_0)
		if arg_29_2 then
			arg_29_2()
		end
	end

	local function var_29_1(arg_31_0)
		if arg_29_3 then
			arg_29_3()
		end

		arg_31_0.is_running = false
	end

	arg_29_0:enable()

	arg_29_0.tween = Tween:sequence(Tween:wait(arg_29_1), Tween:callback(var_29_0, arg_29_0), Tween:alphaTo(arg_29_0.component, CloverConst.Fade.OUT_DURATION, 1), Tween:callback(var_29_1, arg_29_0)):start()
end

local var_0_3 = class(var_0_1)

function var_0_3.init(arg_32_0, arg_32_1)
	var_0_1.init(arg_32_0, arg_32_1)

	local var_32_0 = arg_32_1:getChildByName("RootPane")

	arg_32_0.anims = {}
	arg_32_0.play_waits = {}

	for iter_32_0 in iterate_children(var_32_0) do
		table.insert(arg_32_0.anims, iter_32_0:getChildByName("RootPane"):getComponent(AnimatorComponent))
		table.insert(arg_32_0.play_waits, 0)
	end

	arg_32_0.anim_in = arg_32_1.anim_in
	arg_32_0.anim_out = arg_32_1.anim_out

	arg_32_0:forEachAnim(function(arg_33_0)
		arg_33_0:stop()
		arg_33_0:setLoop(false)
	end)

	arg_32_0.sound_in = arg_32_1.sound_in
	arg_32_0.sound_out = arg_32_1.sound_out
	arg_32_0.callback = nil
	arg_32_0.is_fade_out = false
end

function var_0_3.doUpdate(arg_34_0, arg_34_1)
	if arg_34_0.is_running == false then
		return
	end

	for iter_34_0, iter_34_1 in ipairs(arg_34_0.play_waits) do
		if iter_34_1 > 0 then
			arg_34_0.play_waits[iter_34_0] = arg_34_0.play_waits[iter_34_0] - arg_34_1

			if arg_34_0.play_waits[iter_34_0] <= 0 then
				arg_34_0.play_waits[iter_34_0] = 0

				arg_34_0.anims[iter_34_0]:play()
			end
		end
	end

	if arg_34_0:isAnimStopAll() then
		arg_34_0.is_running = false

		if arg_34_0.callback then
			arg_34_0.callback()
		end

		if arg_34_0.is_fade_out == false then
			arg_34_0:disable()
		end

		arg_34_0.is_fade_out = false
	end
end

function var_0_3.doIn(arg_35_0, arg_35_1, arg_35_2, arg_35_3)
	arg_35_0:enable()

	arg_35_0.is_running = true
	arg_35_0.is_fade_out = false
	arg_35_0.callback = arg_35_3

	local var_35_0 = #arg_35_0.anims
	local var_35_1 = 0.0175

	arg_35_0:forEachAnimIndex(function(arg_36_0, arg_36_1)
		arg_36_0:setSceneAnimation(arg_35_0.anim_in)
		arg_36_0:setCurrentTime(0)
		arg_36_0:setLoop(false)

		local var_36_0 = var_35_0 - arg_36_1 + 1

		arg_36_0:stop()

		arg_35_0.play_waits[arg_36_1] = var_35_1 * var_36_0
	end)
	arg_35_0.sound_in:play()
end

function var_0_3.doOut(arg_37_0, arg_37_1, arg_37_2, arg_37_3)
	arg_37_0:enable()

	arg_37_0.is_running = true
	arg_37_0.is_fade_out = true
	arg_37_0.callback = arg_37_3

	local var_37_0 = 0.0175

	arg_37_0:forEachAnimIndex(function(arg_38_0, arg_38_1)
		arg_38_0:setSceneAnimation(arg_37_0.anim_out)
		arg_38_0:setCurrentTime(0)
		arg_38_0:setLoop(false)

		if arg_38_1 == 1 then
			arg_38_0:play()

			arg_37_0.play_waits[arg_38_1] = 0
		else
			arg_38_0:stop()

			arg_37_0.play_waits[arg_38_1] = var_37_0 * (arg_38_1 - 1)
		end
	end)
	arg_37_0.sound_out:play()
end

function var_0_3.forEachAnim(arg_39_0, arg_39_1)
	for iter_39_0, iter_39_1 in ipairs(arg_39_0.anims) do
		arg_39_1(iter_39_1)
	end
end

function var_0_3.forEachAnimIndex(arg_40_0, arg_40_1)
	for iter_40_0, iter_40_1 in ipairs(arg_40_0.anims) do
		arg_40_1(iter_40_1, iter_40_0)
	end
end

function var_0_3.isAnimStopAll(arg_41_0)
	for iter_41_0, iter_41_1 in ipairs(arg_41_0.anims) do
		if not iter_41_1:isStopped() then
			return false
		end

		if arg_41_0.play_waits[iter_41_0] > 0 then
			return false
		end
	end

	return true
end

var_0_0 = {
	CreateInstance = function(arg_42_0, arg_42_1)
		local var_42_0 = ({
			black = function()
				return new(var_0_2)
			end,
			white = function()
				local var_44_0 = new(var_0_2)

				var_44_0:setColor(1, 1, 1)

				return var_44_0
			end,
			blind = function()
				return new(var_0_3)
			end
		})[arg_42_0]()

		var_42_0:init(arg_42_1)

		return var_42_0
	end
}
