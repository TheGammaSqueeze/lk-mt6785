require("/scripts/core/core.lua")
require("/scripts/app/clover_const.lua")

sys_home_floating_cards = class(WorldNode)

function sys_home_floating_cards.start(arg_1_0)
	arg_1_0.orgX, arg_1_0.orgY = arg_1_0:getLocalPosition()
	arg_1_0.Lx, arg_1_0.Ly = arg_1_0.cardL:getLocalPosition()
	arg_1_0.Mx, arg_1_0.My = arg_1_0.cardM:getLocalPosition()
	arg_1_0.Sx, arg_1_0.Sy = arg_1_0.cardS:getLocalPosition()

	arg_1_0.cardL:disable()
	arg_1_0.cardM:enable()
	arg_1_0.cardS:disable()

	arg_1_0.cardSize = "M"

	if system.floating_save then
		local var_1_0 = system.floating_save.game_code
		local var_1_1 = SuspentionPoints:getInfo(var_1_0, "floating")

		arg_1_0:setInfo(var_1_1)
	else
		arg_1_0:setInfo(nil)
	end

	system.floating_cards = arg_1_0

	if arg_1_0.operation then
		arg_1_0.operation:disable()
	end

	if arg_1_0.fuwafuwa then
		arg_1_0.fuwafuwa:addWing(arg_1_0.cardL.wingL)
		arg_1_0.fuwafuwa:addWing(arg_1_0.cardL.wingR)
		arg_1_0.fuwafuwa:addWing(arg_1_0.cardM.wingL)
		arg_1_0.fuwafuwa:addWing(arg_1_0.cardM.wingR)
		arg_1_0.fuwafuwa:addWing(arg_1_0.cardS.wingL)
		arg_1_0.fuwafuwa:addWing(arg_1_0.cardS.wingR)
		arg_1_0.fuwafuwa:setType("homemenu")
	end

	arg_1_0.is_moving = false
end

function sys_home_floating_cards.setDisable(arg_2_0)
	arg_2_0:setInfo(nil)
	arg_2_0:hideArrow()
end

function sys_home_floating_cards.setInfo(arg_3_0, arg_3_1)
	if arg_3_1 then
		arg_3_0.is_empty = false

		arg_3_0.cardL:setInfo(arg_3_1)
		arg_3_0.cardM:setInfo(arg_3_1)
		arg_3_0.cardS:setInfo(arg_3_1)
	elseif not arg_3_0.is_empty then
		arg_3_0.is_empty = true

		arg_3_0.cardL:disable()
		arg_3_0.cardM:disable()
		arg_3_0.cardS:disable()
	end
end

function sys_home_floating_cards.setSelected(arg_4_0)
	arg_4_0.cardL:setSelected()
	arg_4_0.cardM:setSelected()
	arg_4_0.cardS:setSelected()
	arg_4_0:setSize("M")

	if store.play_count < 2 then
		-- block empty
	end

	arg_4_0.cardL:hideWarning()
end

function sys_home_floating_cards.setUnselected(arg_5_0)
	arg_5_0.cardL:setUnselected()
	arg_5_0.cardM:setUnselected()
	arg_5_0.cardS:setUnselected()
	arg_5_0.cardM:hideWarning()
	arg_5_0:setSize("S")
end

function sys_home_floating_cards.erase(arg_6_0)
	arg_6_0.smoke:enable()

	local var_6_0

	if arg_6_0.cardS:isEnabled() then
		var_6_0 = arg_6_0.cardS

		arg_6_0.smoke:setWorldPosition(arg_6_0.cardS:getWorldPosition())
		arg_6_0.smoke:runS()
	end

	if arg_6_0.cardM:isEnabled() then
		var_6_0 = arg_6_0.cardM

		arg_6_0.smoke:setWorldPosition(arg_6_0.cardM:getWorldPosition())
		arg_6_0.smoke:runM()
	end

	if arg_6_0.cardL:isEnabled() then
		var_6_0 = arg_6_0.cardL

		arg_6_0.smoke:setWorldPosition(arg_6_0.cardL:getWorldPosition())
		arg_6_0.smoke:runL()
	end

	if arg_6_0.fuwafuwa then
		arg_6_0.fuwafuwa:stopAnim()
	end

	if arg_6_0.floatingEraseSound then
		arg_6_0.floatingEraseSound:stop()
		arg_6_0.floatingEraseSound:play()
	end

	arg_6_0.cardL:disable()
	arg_6_0.cardM:disable()
	arg_6_0.cardS:disable()

	local var_6_1, var_6_2 = pcall(system.delete_resumedata, system.floating_save.folder_name)

	if not var_6_1 then
		system.floatingpoint_deletefailed = true
	end

	system.delayFadeOut = 0.2

	if HOST_PLATFORM_IS_WINDOWS then
		Tween:wait(system.delayFadeOut + 0.1):callback(function()
			if var_6_0 then
				var_6_0:enable()
			end
		end):start()
	end
end

function sys_home_floating_cards.moveToResumeList(arg_8_0)
	arg_8_0:setSize("L")
	arg_8_0.cardM:hideWarning()

	if arg_8_0.operation then
		arg_8_0.operation:enable()
	end

	if arg_8_0.fuwafuwa then
		arg_8_0.fuwafuwa:setType("lockmode")
	end
end

function sys_home_floating_cards.moveFromResumeList(arg_9_0)
	arg_9_0:setSize("M")

	if not arg_9_0.cardM.first_move then
		arg_9_0.cardS:setLocalPosition(0, 0)

		local var_9_0 = arg_9_0.orgX
		local var_9_1 = arg_9_0.orgY

		arg_9_0.is_moving = false

		tween_stop(arg_9_0.tween)

		arg_9_0.tween = Tween:parallel(Tween:wait(0.1):moveTo(arg_9_0, 0.3, var_9_0, var_9_1, Ease.outExpo), Tween:wait(0.5):callback(function()
			arg_9_0:setZIndex(0)
		end)):start()
	end

	if arg_9_0.operation then
		arg_9_0.operation:disable()
	end

	if arg_9_0.fuwafuwa then
		arg_9_0.fuwafuwa:setType("homemenu")
	end
end

function sys_home_floating_cards.moveLockMode(arg_11_0, arg_11_1, arg_11_2)
	local var_11_0, var_11_1 = arg_11_0:getParentNode():worldToLocalPosition(arg_11_1:getWorldPosition())

	if system.cursor then
		system.cursor:setRounded(arg_11_0.cardL.cursor_component)
	end

	tween_stop(arg_11_0.tween)

	arg_11_0.is_moving = true
	arg_11_0.tween = Tween:parallel(Tween:wait(0.1):callback(function()
		arg_11_0.is_moving = false
	end), Tween:moveTo(arg_11_0, 0.3, var_11_0, arg_11_2, Ease.outExpo)):start()
end

function sys_home_floating_cards.setActive(arg_13_0)
	if not arg_13_0.cardM.first_move then
		arg_13_0.cardL:setSelected()
		arg_13_0.cardM:setSelected()
		arg_13_0.cardS:setSelected()
		arg_13_0:setZIndex(30)

		if arg_13_0.operation then
			arg_13_0.operation:enable()
		end
	end
end

function sys_home_floating_cards.setDeactive(arg_14_0, arg_14_1)
	if not arg_14_0.cardM.first_move then
		arg_14_0.cardL:setUnselected()
		arg_14_0.cardM:setUnselected()
		arg_14_0.cardS:setUnselected()
		arg_14_0.cardL:hideWarning()
		arg_14_0:setZIndex(0)

		arg_14_0.is_moving = false

		tween_stop(arg_14_0.tween)

		local var_14_0, var_14_1 = arg_14_0:getLocalPosition()

		arg_14_0.tween = Tween:moveTo(arg_14_0, 0.3, var_14_0, arg_14_1, Ease.outExpo):start()

		if arg_14_0.operation then
			arg_14_0.operation:disable()
		end
	end
end

function sys_home_floating_cards.showArrow(arg_15_0)
	if arg_15_0.operation then
		arg_15_0.operation:enable()
	end
end

function sys_home_floating_cards.hideArrow(arg_16_0)
	if arg_16_0.operation then
		arg_16_0.operation:disable()
	end
end

function sys_home_floating_cards.setRegisterLabel(arg_17_0, arg_17_1)
	if arg_17_0.cardL.register_label then
		if arg_17_1 then
			arg_17_0.cardL.register_label:setTextFromStringId("sys_resume_hud_Substitute")
		else
			arg_17_0.cardL.register_label:setTextFromStringId("sys_resume_hud_Register")
		end

		arg_17_0.cardL:refresh()
	end
end

function sys_home_floating_cards.showRegisterFrame(arg_18_0)
	if arg_18_0.cardL.register_node then
		arg_18_0.cardL.register_node:enable()
	end
end

function sys_home_floating_cards.hideRegisterFrame(arg_19_0)
	if arg_19_0.cardL.register_node then
		arg_19_0.cardL.register_node:disable()
	end
end

function sys_home_floating_cards.disableWing(arg_20_0)
	if arg_20_0.operation then
		arg_20_0.operation:disable()
	end

	arg_20_0.cardL.wingL:disable()
	arg_20_0.cardL.wingR:disable()

	if arg_20_0.fuwafuwa then
		arg_20_0.fuwafuwa:stopAnim()
	end

	if arg_20_0.fuwafuwa then
		arg_20_0.fuwafuwa:setType("stop")
	end
end

function sys_home_floating_cards.stopSizeAnim(arg_21_0)
	tween_stop(arg_21_0.setSizeTween)
end

function sys_home_floating_cards.setSize(arg_22_0, arg_22_1)
	local var_22_0 = 0.4
	local var_22_1 = 1.5
	local var_22_2 = 1.2
	local var_22_3 = 1.7
	local var_22_4 = Ease.outExpo

	if arg_22_0.cardSize == "M" and arg_22_1 == "S" then
		arg_22_0.cardSize = arg_22_1

		arg_22_0.cardL:disable()
		arg_22_0.cardS:enable()
		arg_22_0.cardM:disable()
		arg_22_0.cardS:setLocalScale(var_22_1, var_22_1)
		arg_22_0.cardS:setLocalPosition(arg_22_0.Mx, arg_22_0.My)
		tween_stop(arg_22_0.setSizeTween)

		arg_22_0.setSizeTween = Tween:parallel(Tween:moveTo(arg_22_0.cardS, var_22_0, arg_22_0.Sx, arg_22_0.Sy, Ease.outExpo), Tween:scaleTo(arg_22_0.cardS, var_22_0, 1, 1, var_22_4)):start()
	elseif arg_22_0.cardSize == "S" and arg_22_1 == "M" then
		arg_22_0.cardSize = arg_22_1

		arg_22_0.cardL:disable()
		arg_22_0.cardS:disable()
		arg_22_0.cardM:enable()
		arg_22_0.cardM:setLocalScale(1 / var_22_1, 1 / var_22_1)
		arg_22_0.cardM:setLocalPosition(arg_22_0.Sx, arg_22_0.Sy)
		tween_stop(arg_22_0.setSizeTween)

		arg_22_0.setSizeTween = Tween:parallel(Tween:moveTo(arg_22_0.cardM, var_22_0, arg_22_0.Mx, arg_22_0.My, Ease.outExpo), Tween:scaleTo(arg_22_0.cardM, var_22_0, 1, 1, var_22_4)):start()
	elseif arg_22_0.cardSize == "M" and arg_22_1 == "L" then
		arg_22_0.cardSize = arg_22_1

		arg_22_0.cardL:enable()
		arg_22_0.cardS:disable()
		arg_22_0.cardM:disable()
		arg_22_0.cardL:setLocalScale(1, 1)
		arg_22_0.cardL:setLocalPosition(arg_22_0.Mx, arg_22_0.My)
		tween_stop(arg_22_0.setSizeTween)

		arg_22_0.setSizeTween = Tween:parallel(Tween:moveTo(arg_22_0.cardL, var_22_0, arg_22_0.Lx, arg_22_0.Ly, Ease.outExpo)):start()
	elseif arg_22_0.cardSize == "L" and arg_22_1 == "M" then
		arg_22_0.cardSize = arg_22_1

		arg_22_0.cardL:disable()
		arg_22_0.cardS:disable()
		arg_22_0.cardM:enable()
		arg_22_0.cardM:setLocalScale(var_22_2, var_22_2)
		arg_22_0.cardM:setLocalPosition(arg_22_0.Lx, arg_22_0.Ly)
		tween_stop(arg_22_0.setSizeTween)

		arg_22_0.setSizeTween = Tween:parallel(Tween:moveTo(arg_22_0.cardM, var_22_0, arg_22_0.Mx, arg_22_0.My, Ease.outExpo), Tween:scaleTo(arg_22_0.cardM, var_22_0, 1, 1, var_22_4)):start()
	end

	arg_22_0.cardS:updateTexture()
	arg_22_0.cardM:updateTexture()
	arg_22_0.cardL:updateTexture()
end

function sys_home_floating_cards.expandAnimation(arg_23_0)
	arg_23_0:hideArrow()
	arg_23_0:hideRegisterFrame()
	arg_23_0.cardL:expandAnimation()

	if HOST_PLATFORM_IS_WINDOWS then
		Tween:wait(system.delayFadeOut or 0):wait(0.4):callback(function()
			arg_23_0:showArrow()
		end):start()
	end
end

function sys_home_floating_cards.sramIconRefresh(arg_25_0)
	arg_25_0.cardL:sramIconRefresh()
end

function sys_home_floating_cards.isMoving(arg_26_0)
	return arg_26_0.is_moving
end
