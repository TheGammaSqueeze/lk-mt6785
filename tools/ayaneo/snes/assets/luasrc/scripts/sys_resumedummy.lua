require("/scripts/modules/core/core.lua")
require("/scripts/tween/tween.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/app/clover_util.lua")

sys_resumedummy = class(WorldNode)

function sys_resumedummy.start(arg_1_0)
	arg_1_0:disable()

	arg_1_0.is_select_title = false
end

function sys_resumedummy.stop(arg_2_0)
	return
end

function sys_resumedummy.update(arg_3_0, arg_3_1)
	return
end

function sys_resumedummy.selectTitle(arg_4_0)
	local var_4_0 = not sys_boot:isUpdateMain()
	local var_4_1 = CloverConst.Suspension.DUMMY_DELAY_USUAL

	if var_4_0 then
		var_4_1 = CloverConst.Suspension.DUMMY_DELAY_FIRST

		if system.isPlayingDemo() and PREVIOUS_COMMAND == "super-play" then
			return
		end
	elseif system.isPlayingDemo() and not CloverDemo.running_cancel then
		return
	end

	arg_4_0:activate(var_4_1)

	arg_4_0.is_select_title = true
	arg_4_0.is_resumeon = false
	arg_4_0.org_z_index = arg_4_0:getZIndex()
end

function sys_resumedummy.unselectTitle(arg_5_0)
	arg_5_0:deactivate()

	arg_5_0.is_select_title = false
end

function sys_resumedummy.focusMenubar(arg_6_0)
	if arg_6_0.is_select_title then
		arg_6_0:deactivate()
	end
end

function sys_resumedummy.resumeOn(arg_7_0)
	arg_7_0.is_resumeon = true

	if arg_7_0.is_select_title then
		tween_stop(arg_7_0.resume_tween)

		arg_7_0.resume_tween = Tween:worldNodeEnabledTo(arg_7_0.root, 0.025, false):start()
	end
end

function sys_resumedummy.resumeOff(arg_8_0)
	arg_8_0.is_resumeon = false

	if not system.isExistsFloatingData() then
		arg_8_0.is_select_title = false

		arg_8_0:disable()
		tween_stop(arg_8_0.resume_tween)
		tween_stop(arg_8_0.tween)
	end

	if arg_8_0.is_select_title then
		tween_stop(arg_8_0.resume_tween)

		arg_8_0.resume_tween = Tween:worldNodeEnabledTo(arg_8_0.root, 0.175, true):start()
	end
end

function sys_resumedummy.startDemo(arg_9_0)
	arg_9_0:unselectTitle()
end

function sys_resumedummy.activate(arg_10_0, arg_10_1)
	if arg_10_0:isEnabled() then
		return
	end

	tween_stop(arg_10_0.tween)
	arg_10_0:setLocalPosition(0, CloverConst.Suspension.DUMMY_OUT_POS_Y)

	arg_10_0.tween = Tween:sequence(Tween:worldNodeEnabledTo(arg_10_0, arg_10_1, true), Tween:moveTo(arg_10_0, 0.125, 0, CloverConst.Suspension.DUMMY_IN_POS_Y)):start()
end

function sys_resumedummy.deactivate(arg_11_0)
	tween_stop(arg_11_0.tween)

	if not arg_11_0:isEnabled() then
		return
	end

	arg_11_0.tween = Tween:sequence(Tween:moveTo(arg_11_0, 0.125, 0, CloverConst.Suspension.DUMMY_OUT_POS_Y), Tween:worldNodeEnabledTo(arg_11_0, 0, false)):start()
end
