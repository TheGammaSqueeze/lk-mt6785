require("/scripts/core/core.lua")
require("/scripts/core/Math.lua")
require("/scripts/app/clover_util.lua")
require("/scripts/app/clover_const.lua")

local var_0_0 = {}

CloverCharacter = class()

function CloverCharacter.new(arg_1_0, arg_1_1)
	local var_1_0 = new(CloverCharacter)

	var_1_0:init(arg_1_0, arg_1_1)

	return var_1_0
end

function CloverCharacter.init(arg_2_0, arg_2_1, arg_2_2)
	arg_2_0.root = arg_2_1:getChildByName(arg_2_2)
	arg_2_0.anim = arg_2_0.root:getChildByName("RootPane"):getComponent(AnimatorComponent)

	arg_2_0.anim:stop()

	arg_2_0.walk_request = false
	arg_2_0.is_walk = false
	arg_2_0.walk_to_x = 0
	arg_2_0.request_anim = ""
	arg_2_0.keep_pose = false
	arg_2_0.current_anim = ""
	arg_2_0.state = CloverState.new(arg_2_0)

	arg_2_0.state:add("stay", CloverCharacter.stay_state)
	arg_2_0.state:add("walk", CloverCharacter.walk_state)
	arg_2_0.state:add("jump", CloverCharacter.jump_state)
	arg_2_0.state:add("oneshot_anim", CloverCharacter.oneshot_anim_state)
	arg_2_0.state:add("exit", CloverCharacter.exit_state)
	arg_2_0.state:change("stay")

	arg_2_0.request = {}
end

function CloverCharacter.update(arg_3_0, arg_3_1)
	arg_3_0.state:update(arg_3_1)
end

function CloverCharacter.SetVisible(arg_4_0, arg_4_1)
	arg_4_0.root:setVisible(arg_4_1)
end

function CloverCharacter.SetKeepPose(arg_5_0, arg_5_1)
	if arg_5_0:IsStay() then
		return
	end

	arg_5_0.keep_pose = arg_5_1
end

function CloverCharacter.SetWorldZIndex(arg_6_0, arg_6_1)
	arg_6_0.root:setWorldZIndex(arg_6_1)
end

function CloverCharacter.GetLocalPosition(arg_7_0)
	return arg_7_0.root:getLocalPosition()
end

function CloverCharacter.SetLocalPosition(arg_8_0, arg_8_1, arg_8_2)
	arg_8_0.root:setLocalPosition(arg_8_1, arg_8_2)
end

function CloverCharacter.GetWorldPosition(arg_9_0)
	return arg_9_0.root:getWorldPosition()
end

function CloverCharacter.SetWorldPosition(arg_10_0, arg_10_1, arg_10_2)
	arg_10_0.root:setWorldPosition(arg_10_1, arg_10_2)
end

function CloverCharacter.OneshotAnim(arg_11_0, arg_11_1, arg_11_2, arg_11_3)
	assert(arg_11_3 or arg_11_0:IsStay())

	arg_11_0.request_anim = arg_11_1
	arg_11_0.keep_pose = arg_11_2

	if not wiat_stay or arg_11_0:IsStay() then
		arg_11_0.state:set_next("oneshot_anim")
	end
end

function CloverCharacter.Stay(arg_12_0)
	arg_12_0.state:set_next("stay")
end

function CloverCharacter.Walk(arg_13_0, arg_13_1, arg_13_2, arg_13_3, arg_13_4)
	arg_13_0.request = {
		walk_target_x = arg_13_1
	}

	if arg_13_2 == nil then
		arg_13_0.request.walk_look_x = arg_13_1
	else
		arg_13_0.request.walk_look_x = arg_13_2
	end

	if arg_13_3 == nil then
		arg_13_0.request.walk_lowest_time = CloverConst.Chr.LOWEST_WALK_TIME
	else
		arg_13_0.request.walk_lowest_time = arg_13_3
	end

	if arg_13_4 == nil then
		arg_13_0.request.walk_is_slow = false
	else
		arg_13_0.request.walk_is_slow = arg_13_4
	end

	arg_13_0.state:set_next("walk")
end

function CloverCharacter.UpdateWalkTarget(arg_14_0, arg_14_1, arg_14_2)
	if not arg_14_0:IsWalk() then
		return
	end

	arg_14_0.request.walk_target_x = arg_14_1

	if arg_14_2 == nil then
		arg_14_0.request.walk_look_x = arg_14_1
	else
		arg_14_0.request.walk_look_x = arg_14_2
	end

	arg_14_0:LookTo(arg_14_0.request.walk_look_x)
end

function CloverCharacter.Jump(arg_15_0, arg_15_1, arg_15_2)
	local var_15_0, var_15_1 = arg_15_0.root:getWorldPosition()

	arg_15_0.request = {}

	if arg_15_2 == nil then
		arg_15_0.request.jump_target_y = var_15_1
	else
		arg_15_0.request.jump_target_y = arg_15_2
	end

	if arg_15_1 == nil then
		arg_15_0.request.jump_target_x = var_15_0
	else
		arg_15_0.request.jump_target_x = arg_15_1
	end

	arg_15_0.request.jump_start_x = var_15_0
	arg_15_0.request.jump_start_y = var_15_1

	arg_15_0.state:set_next("jump")
end

function CloverCharacter.Exit(arg_16_0)
	arg_16_0.state:set_next("exit")
end

function CloverCharacter.LookTo(arg_17_0, arg_17_1)
	local var_17_0, var_17_1 = arg_17_0.root:getWorldPosition()

	if var_17_0 < arg_17_1 then
		arg_17_0.root:setLocalScaleX(-1)
	end

	if arg_17_1 < var_17_0 then
		arg_17_0.root:setLocalScaleX(1)
	end
end

function CloverCharacter.LookLeft(arg_18_0)
	arg_18_0.root:setLocalScaleX(1)
end

function CloverCharacter.LookRight(arg_19_0)
	arg_19_0.root:setLocalScaleX(-1)
end

function CloverCharacter.IsStay(arg_20_0)
	return var_0_0.CheckState(arg_20_0, "stay")
end

function CloverCharacter.IsWalk(arg_21_0)
	return var_0_0.CheckState(arg_21_0, "walk")
end

function CloverCharacter.IsNeedWalk(arg_22_0, arg_22_1)
	local var_22_0, var_22_1 = arg_22_0.root:getWorldPosition()

	if math.abs(var_22_0 - arg_22_1) < 0.5 then
		return false
	end

	return true
end

function CloverCharacter.stay_state(arg_23_0, arg_23_1)
	if arg_23_1:is_first() then
		if arg_23_0.request_anim == "" then
			if arg_23_0.keep_pose == false then
				var_0_0.ChangeAnim(arg_23_0, "wait", false)
			end

			arg_23_0.keep_pose = false
		else
			var_0_0.ChangeAnim(arg_23_0, "wait", false)
			arg_23_1:set_next("oneshot_anim")
		end
	end
end

function CloverCharacter.walk_state(arg_24_0, arg_24_1)
	if arg_24_1:is_first() then
		if arg_24_0.root.run then
			var_0_0.ChangeAnim(arg_24_0, "run", true)
		end

		arg_24_0:LookTo(arg_24_0.request.walk_look_x)

		if arg_24_0.request.walk_is_slow then
			arg_24_0.anim:setSpeed(0.5)
		end
	end

	local var_24_0, var_24_1 = arg_24_0.root:getWorldPosition()
	local var_24_2 = arg_24_0.request.walk_target_x
	local var_24_3 = var_0_0.LinearInterp(var_24_0, var_24_2, CloverConst.Chr.WALK_SPEED, arg_24_1.dt)

	arg_24_0.root:setWorldPosition(var_24_3, var_24_1)

	if var_24_2 == var_24_3 and arg_24_1.passed_time > arg_24_0.request.walk_lowest_time then
		arg_24_1:set_next("stay")
	end
end

function CloverCharacter.jump_state(arg_25_0, arg_25_1)
	if arg_25_1:is_first() then
		var_0_0.ChangeAnim(arg_25_0, "jump", false)
		arg_25_0:LookTo(arg_25_0.request.jump_target_x)
	end

	local var_25_0 = arg_25_0.anim:getCurrentTime() / (arg_25_0.anim:getDuration() - 0.125)

	if var_25_0 > 1 then
		var_25_0 = 1
	end

	local var_25_1 = arg_25_0.anim:getCurrentTime() / (arg_25_0.anim:getDuration() - 0.175)

	if var_25_1 > 1 then
		var_25_1 = 1
	end

	local var_25_2 = Math.lerp(arg_25_0.request.jump_start_x, arg_25_0.request.jump_target_x, var_25_0)
	local var_25_3 = Math.lerp(arg_25_0.request.jump_start_y, arg_25_0.request.jump_target_y, var_25_1)

	arg_25_0.root:setWorldPosition(var_25_2, var_25_3)

	if arg_25_0.anim:isStopped() then
		arg_25_1:set_next("stay")
		arg_25_0.root:setWorldPosition(arg_25_0.request.jump_target_x, arg_25_0.request.jump_target_y)
	end
end

function CloverCharacter.oneshot_anim_state(arg_26_0, arg_26_1)
	if arg_26_1:is_first() then
		var_0_0.ChangeAnim(arg_26_0, arg_26_0.request_anim, false)

		arg_26_0.request_anim = ""
	end

	if arg_26_0.anim:isStopped() then
		arg_26_1:set_next("stay")
	end
end

function CloverCharacter.exit_state(arg_27_0, arg_27_1)
	if arg_27_1:is_first() then
		-- block empty
	end

	local var_27_0 = arg_27_0.root:getAlpha() - arg_27_1.dt * CloverConst.Chr.ALPHA_SPEED_RATE

	if var_27_0 > 0 then
		arg_27_0.root:setAlpha(var_27_0)
	else
		arg_27_0.root:setAlpha(1)
		arg_27_0:SetVisible(false)
		arg_27_1:set_next("stay")
	end
end

CloverMario = class(CloverCharacter)

function CloverMario.new(arg_28_0, arg_28_1)
	local var_28_0 = new(CloverMario)

	var_28_0:init(arg_28_0, arg_28_1)

	return var_28_0
end

function CloverMario.init(arg_29_0, arg_29_1, arg_29_2)
	CloverCharacter.init(arg_29_0, arg_29_1, arg_29_2)
	arg_29_0.state:add("spin", CloverMario.spin_state)
	arg_29_0.state:add("ride", CloverMario.ride_state)
end

function CloverMario.Spin(arg_30_0, arg_30_1)
	arg_30_0.request = {}
	arg_30_0.request.spin_move_x = arg_30_1

	arg_30_0.state:set_next("spin")
end

function CloverMario.Ride(arg_31_0, arg_31_1)
	local var_31_0, var_31_1 = arg_31_0.root:getWorldPosition()

	arg_31_0.request = {}
	arg_31_0.request.ride_start_x = var_31_0

	if arg_31_1 == nil then
		arg_31_0.request.ride_target_x = var_31_0
	else
		arg_31_0.request.ride_target_x = arg_31_1
	end

	arg_31_0.state:set_next("ride")
end

function CloverMario.spin_state(arg_32_0, arg_32_1)
	if arg_32_1:is_first() then
		var_0_0.ChangeAnim(arg_32_0, "spin", false)
	end

	if arg_32_0.request.spin_move_x and arg_32_1.passed_time < CloverConst.Chr.SPIN_MOVE_TIME then
		local var_32_0, var_32_1 = arg_32_0.root:getWorldPosition()
		local var_32_2 = var_32_0 + arg_32_0.request.spin_move_x * arg_32_1.dt

		arg_32_0.root:setWorldPosition(var_32_2, var_32_1)
	end

	if arg_32_0.anim:isStopped() then
		arg_32_0:SetVisible(false)
		arg_32_1:set_next("stay")
	end
end

function CloverMario.ride_state(arg_33_0, arg_33_1)
	if arg_33_1:is_first() then
		var_0_0.ChangeAnim(arg_33_0, "ride", false)
		arg_33_0:LookTo(arg_33_0.request.ride_target_x)
	end

	local var_33_0, var_33_1 = arg_33_0.root:getWorldPosition()
	local var_33_2 = arg_33_0.anim:getCurrentTime() / (arg_33_0.anim:getDuration() - 0.125)

	if var_33_2 > 1 then
		var_33_2 = 1
	end

	local var_33_3 = Math.lerp(arg_33_0.request.ride_start_x, arg_33_0.request.ride_target_x, var_33_2)

	arg_33_0.root:setWorldPosition(var_33_3, var_33_1)

	if arg_33_0.anim:isStopped() then
		arg_33_1:set_next("stay")

		local var_33_4, var_33_5 = arg_33_0.root.pos_pane:getWorldPosition()

		var_0_0.ChangeAnim(arg_33_0, "wait", false)
		arg_33_0.root:setWorldPosition(var_33_3, var_33_5)
	end
end

function var_0_0.ChangeAnim(arg_34_0, arg_34_1, arg_34_2)
	arg_34_0.anim:setSceneAnimation(arg_34_0.root[arg_34_1])
	arg_34_0.anim:setCurrentTime(0)
	arg_34_0.anim:setLoop(arg_34_2)
	arg_34_0.anim:setSpeed(1)
	arg_34_0.anim:play()

	arg_34_0.current_anim = arg_34_1
end

function var_0_0.CheckState(arg_35_0, arg_35_1)
	if arg_35_0.state.current_state ~= arg_35_1 then
		return false
	end

	if arg_35_0.state.next_state ~= arg_35_1 then
		return false
	end

	return true
end

function var_0_0.LinearInterp(arg_36_0, arg_36_1, arg_36_2, arg_36_3)
	if arg_36_0 == arg_36_1 then
		return arg_36_0
	end

	if arg_36_0 < arg_36_1 then
		arg_36_0 = arg_36_0 + arg_36_3 * arg_36_2

		if arg_36_1 < arg_36_0 then
			arg_36_0 = arg_36_1
		end
	end

	if arg_36_1 < arg_36_0 then
		arg_36_0 = arg_36_0 - arg_36_3 * arg_36_2

		if arg_36_0 < arg_36_1 then
			arg_36_0 = arg_36_1
		end
	end

	return arg_36_0
end
