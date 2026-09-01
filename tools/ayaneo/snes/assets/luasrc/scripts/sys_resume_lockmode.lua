require("/scripts/core/core.lua")
require("/scripts/suspentionpoints.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/app/clover_autocancel.lua")

sys_resume_lockmode = class(gui_container)

function sys_resume_lockmode.start(arg_1_0)
	gui_container.start(arg_1_0)

	local var_1_0, var_1_1 = arg_1_0:getWorldPosition()

	arg_1_0.floating_y = var_1_1
end

function sys_resume_lockmode.stop(arg_2_0)
	gui_container.stop(arg_2_0)
end

function sys_resume_lockmode.update(arg_3_0, arg_3_1)
	gui_container.update(arg_3_0, arg_3_1)

	if arg_3_0:elementIsFocused() and not GUI.disabled and arg_3_0.longpress_delay ~= nil then
		if arg_3_0.longpress_delay < CloverConst.Suspension.OVERWRITE_PRESS_MARGIN_SECONDS then
			arg_3_0.longpress_delay = arg_3_0.longpress_delay + arg_3_1
		elseif CloverPad1P.down[Pad.register_btn] then
			arg_3_0.longpress_delay = arg_3_0.longpress_delay + arg_3_1

			if arg_3_0.longpress_delay >= CloverConst.Suspension.OVERWRITE_COMPLETE_SECONDS then
				arg_3_0.longpress_delay = nil
				arg_3_0.longpress_isFull = true

				arg_3_0:lockCard_overwrite()
				CloverPadUI:EnableUserInput(true, "overwrite")
			end
		else
			arg_3_0:setGaugeAnimStop(true)
			CloverPadUI:EnableUserInput(true, "overwrite")
		end
	end
end

function sys_resume_lockmode.setGaugeAnim(arg_4_0)
	if arg_4_0.pushSound then
		arg_4_0.pushSound:stop()
		arg_4_0.pushSound:play()
	end

	local var_4_0 = CloverConst.Suspension.OVERWRITE_COMPLETE_SECONDS
	local var_4_1 = system.floating_cards.cardL

	tween_stop(arg_4_0.tween)

	local var_4_2 = CloverConst.Suspension.OVERWRITE_FIRST_DOWN
	local var_4_3 = CloverConst.Suspension.OVERWRITE_SECOND_ADD_DOWN

	var_4_1:setLocalPosition(0, var_4_2)

	arg_4_0.tween = Tween:parallel(Tween:parallel(Tween:moveTo(var_4_1, var_4_0, 0, var_4_2 + var_4_3), Tween:moveTo(arg_4_0.toCard.saved_node, var_4_0, 0, var_4_3)), Tween:wait(0.15):loop(math.huge, Tween:sequence(Tween:moveTo(var_4_1.saved_node, 0.05, -3, 0), Tween:moveTo(var_4_1.saved_node, 0.05, 3, 0)))):start()

	system.floating_cards:hideArrow()
end

function sys_resume_lockmode.setGaugeAnimStop(arg_5_0, arg_5_1)
	arg_5_0.longpress_delay = nil

	tween_stop(arg_5_0.tween)

	local var_5_0 = system.floating_cards.cardL

	system.floating_cards.fuwafuwa:setType("stop")
	arg_5_0.toCard.saved_node:setLocalPosition(0, 0)

	if arg_5_1 then
		tween_stop(system.tween)

		system.tween = Tween_noControl(Tween:moveTo(var_5_0, 0.2, 0, 0, Ease.outSine), Tween:callback(function()
			var_5_0:setLocalPosition(0, 0)
			system.floating_cards.fuwafuwa:setType("lockmode")
			system.floating_cards:showArrow()
			system.floating_cards:showRegisterFrame()
		end)):start()
	else
		var_5_0:setLocalPosition(0, 0)
		system.floating_cards.fuwafuwa:setType("lockmode")
		system.floating_cards:showArrow()
		system.floating_cards:showRegisterFrame()
	end
end

function sys_resume_lockmode.openResumeDataList(arg_7_0, arg_7_1)
	arg_7_0.current = arg_7_0.elementArray[arg_7_1]
end

function sys_resume_lockmode.activate(arg_8_0)
	gui_container.activate(arg_8_0)
	system.floating_cards:setActive()
	system.floating_cards.fuwafuwa:setType("lockmode")
	CloverAutoCancel.Set("sys_resume_lockmode", arg_8_0)

	arg_8_0.canceled = false
end

function sys_resume_lockmode.deactivate(arg_9_0)
	gui_container.deactivate(arg_9_0)
	system.floating_cards:setDeactive(arg_9_0.floating_y - 40)
	system.floating_cards.fuwafuwa:setType("lockback")
	system.floating_cards:hideArrow()
	system.floating_cards:hideRegisterFrame()
end

function sys_resume_lockmode.onElementFocus(arg_10_0, arg_10_1)
	gui_container.onElementFocus(arg_10_0, arg_10_1)

	if arg_10_1 and arg_10_1 ~= arg_10_0 then
		local var_10_0 = table_find(arg_10_0.elementArray, arg_10_1)

		if var_10_0 then
			system.floating_cards:moveLockMode(arg_10_1, arg_10_0.floating_y)

			local var_10_1 = arg_10_0.menu:getCard(var_10_0)

			system.floating_cards:setRegisterLabel(not var_10_1:isEmpty())
			system.floating_cards:showArrow()
			system.floating_cards:showRegisterFrame()
			arg_10_0.menu.cardlist:resetCurrent(var_10_0)
			arg_10_0.menu:saveCursorIndex(var_10_0)
		end
	end
end

function sys_resume_lockmode.onElementUnfocus(arg_11_0, arg_11_1)
	gui_container.onElementUnfocus(arg_11_0, arg_11_1)

	if arg_11_0.longpress_delay then
		arg_11_0:setGaugeAnimStop()
	end
end

function sys_resume_lockmode.onButtonClick_(arg_12_0, arg_12_1)
	if arg_12_0.canceled then
		return
	end

	local var_12_0 = arg_12_0.current
	local var_12_1 = table_find(arg_12_0.elementArray, var_12_0)

	if var_12_1 then
		local var_12_2 = arg_12_0.menu:getCard(var_12_1)

		system.floating_cards:hideRegisterFrame()
		system.floating_cards:hideArrow()
		system.floating_cards:stopSizeAnim()

		if var_12_2:isEmpty() then
			arg_12_0:saveAnimation(var_12_0, var_12_2, function()
				local var_13_0, var_13_1 = pcall(function()
					arg_12_0.menu:lockCard(var_12_1)
				end)

				var_13_0 = var_13_0 and coroutine.yield()

				arg_12_0.menu:lockFloatingCardResult(var_12_1, var_13_0)

				if var_13_0 then
					system.cursor:cursorHide()
					arg_12_0.menu:toCardList()
				end

				if not var_13_0 then
					arg_12_0.err_dialog:showDialog(nil)
				end

				return var_13_0
			end)
		elseif not var_12_2:isLocked() then
			arg_12_0.longpress_delay = 0

			system.floating_cards.fuwafuwa:setType("stop")

			arg_12_0.toCard = var_12_2

			arg_12_0:setGaugeAnim()
			CloverPadUI:EnableUserInput(false, "overwrite")
		else
			arg_12_0:overwriteFailedAnimation(var_12_0, var_12_2)
		end
	end
end

function sys_resume_lockmode.lockCard_overwrite(arg_15_0)
	local var_15_0 = arg_15_0.current
	local var_15_1 = table_find(arg_15_0.elementArray, var_15_0)
	local var_15_2 = arg_15_0.menu:getCard(var_15_1)

	system.floating_cards:hideRegisterFrame()
	system.floating_cards:hideArrow()
	arg_15_0:overwriteAnimation(var_15_0, var_15_2, function()
		local var_16_0, var_16_1 = pcall(function()
			arg_15_0.menu:lockCard(var_15_1)
		end)

		var_16_0 = var_16_0 and coroutine.yield()

		arg_15_0.menu:lockFloatingCardResult(var_15_1, var_16_0)

		if var_16_0 then
			system.cursor:cursorHide()
			arg_15_0.menu:toCardList()
		end

		if not var_16_0 then
			arg_15_0.err_dialog:showDialog(nil)
		end

		return var_16_0
	end)
end

function sys_resume_lockmode.onElementCommand(arg_18_0, arg_18_1, arg_18_2)
	local var_18_0 = arg_18_0.elementArray

	if table_find(var_18_0, arg_18_1) and arg_18_2.id == GUI_COMMAND_KEYPRESS then
		if (arg_18_0.upElement == nil or not arg_18_0.upElement:isEnabled()) and CloverPadUI.pressed.up then
			arg_18_0:Back()

			return true
		end

		if not system.floating_cards:isMoving() then
			if CloverPadUI.pressed.btn_start or CloverPadUI.pressed.btn_x then
				local var_18_1 = system.floating_save

				if var_18_1 then
					local var_18_2 = var_18_1.game_code
					local var_18_3 = var_18_1.folder_name .. "/rollback/"
					local var_18_4 = true
					local var_18_5 = true
					local var_18_6 = var_18_1.folder_name .. "/state.time"
					local var_18_7 = CloverPadUI.pressed.btn_x

					if not var_18_7 and arg_18_0.startSound then
						arg_18_0.startSound:stop()
						arg_18_0.startSound:play()
					end

					if var_18_7 then
						arg_18_0:onBootRollback()
						CloverElements.gametitlelist:showReplayText()
					end

					CloverPadUI:EnableUserInput(false, "run")
					system.run_gameresume(var_18_2, var_18_3, var_18_7, var_18_4, var_18_5, var_18_6)

					if not var_18_7 then
						system.floating_cards:expandAnimation()
					end
				end

				return true
			end

			if CloverPadUI.pressed[Pad.register_btn] then
				arg_18_0:onButtonClick_(arg_18_1)

				return
			end
		end
	end

	return gui_container.onElementCommand(arg_18_0, arg_18_1, arg_18_2)
end

function sys_resume_lockmode.onContainerCanceled(arg_19_0, arg_19_1)
	arg_19_0:Back()
end

function sys_resume_lockmode.Back(arg_20_0, arg_20_1)
	arg_20_0.canceled = true

	CloverAutoCancel.Set("sys_resume_lockmode", nil)

	if arg_20_0.longpress_delay then
		arg_20_0:setGaugeAnimStop()
	end

	Main:toHomeMenu()

	if arg_20_0.cancelSound then
		arg_20_0.cancelSound:stop()
		arg_20_0.cancelSound:play()
	end
end

function sys_resume_lockmode.saveAnimation(arg_21_0, arg_21_1, arg_21_2, arg_21_3)
	local var_21_0 = system.floating_cards.cardL
	local var_21_1, var_21_2 = arg_21_2:getWorldPosition()

	if arg_21_0.successSound then
		arg_21_0.successSound:stop()
		arg_21_0.successSound:play()
	end

	system.floating_cards:hideArrow()
	tween_stop(system.tween)

	system.tween = Tween_noControl(Tween:callback(function()
		system.floating_cards.fuwafuwa:setType("stop")
	end), Tween:animate(var_21_0, 0.1, "to", {
		WorldX = var_21_1,
		WorldY = var_21_2
	}, Ease.outSine), Tween:coroutineCallback(function()
		local var_23_0 = true

		if arg_21_3 then
			var_23_0 = arg_21_3()
		end

		if not var_23_0 then
			system.floating_cards.fuwafuwa:setType("lockmode")
			var_21_0:setLocalPosition(0, 0)
		end
	end)):start()
end

function sys_resume_lockmode.overwriteAnimation(arg_24_0, arg_24_1, arg_24_2, arg_24_3)
	tween_stop(arg_24_0.tween)

	local var_24_0 = system.floating_cards.cardL
	local var_24_1, var_24_2 = arg_24_2:getWorldPosition()

	if arg_24_0.successSound then
		arg_24_0.successSound:stop()
		arg_24_0.successSound:play()
	end

	system.floating_cards:hideArrow()
	system.floating_cards:hideRegisterFrame()
	system.floating_cards.fuwafuwa:setType("stop")

	arg_24_0.overwrite = {}

	var_24_0.saved_node:setLocalPosition(0, 0)

	arg_24_0.overwrite.anim_tween = Tween:parallel(Tween_worldTo(var_24_0, 0.02, var_24_1, var_24_2 + 20, Ease.outSine), Tween:wait(0.01):sequence(Tween:rotateTo(arg_24_2.saved_node, 0, math.rad(10)):moveTo(arg_24_2.saved_node, 0.2, 0, -280, Ease.outSine)), Tween:wait(0.060000000000000005):sequence(Tween_worldTo(var_24_0, 0, var_24_1, var_24_2, Ease.outSine), Tween:callback(function()
		system.floating_cards:disableWing()
		var_24_0:setLocalScale(1.1, 1.1)
	end), Tween:wait(0.02), Tween:scaleTo(var_24_0, 0.07, 1, 1, Ease.outSine))):start()

	tween_stop(system.tween)

	system.tween = Tween_noControl(Tween:coroutineCallback(function()
		local var_26_0 = true

		if arg_24_3 then
			var_26_0 = arg_24_3()
		end

		if arg_24_0.overwrite and arg_24_0.overwrite.anim_tween then
			tween_stop(arg_24_0.overwrite.anim_tween)
		end

		arg_24_2.saved_node:setLocalPosition(0, 0)
		arg_24_2.saved_node:setLocalRotation(0)
		var_24_0:setLocalScale(1, 1)

		if not var_26_0 then
			system.floating_cards.fuwafuwa:setType("lockmode")
			var_24_0:setLocalPosition(0, 0)
		end

		arg_24_0.overwrite = {}
	end)):start()
end

function sys_resume_lockmode.overwriteFailedAnimation(arg_27_0, arg_27_1, arg_27_2, arg_27_3)
	local var_27_0 = system.floating_cards.cardL
	local var_27_1, var_27_2 = arg_27_2:getWorldPosition()
	local var_27_3, var_27_4 = arg_27_1:getWorldPosition()

	local function var_27_5()
		if arg_27_0.failureSound then
			arg_27_0.failureSound:stop()
			arg_27_0.failureSound:play()
		end
	end

	system.floating_cards:hideArrow()
	system.floating_cards:hideRegisterFrame()
	tween_stop(system.tween)

	system.tween = Tween_noControl(Tween:sequence(Tween:callback(function()
		system.floating_cards.fuwafuwa:setType("stop")
	end), Tween:animate(var_27_0, 0.1, "to", {
		WorldX = var_27_1,
		WorldY = var_27_2 + 126
	}, Ease.inExpo), Tween:parallel(Tween:callback(function()
		var_27_5()
		arg_27_2:showLockIcon()
	end), Tween:animate(var_27_0, 0.2, "to", {
		WorldX = var_27_3,
		WorldY = var_27_4
	}, Ease.outSine)), Tween:callback(function()
		system.floating_cards.fuwafuwa:setType("lockmode")
		system.floating_cards:showArrow()
		system.floating_cards:showRegisterFrame()

		if arg_27_3 then
			arg_27_3()
		end
	end))):start()
end

function sys_resume_lockmode.onBootRollback(arg_32_0)
	local var_32_0 = table_find(arg_32_0.elementArray, arg_32_0.current)

	store.rollback.lockmode_index = var_32_0
end

function sys_resume_lockmode.onReturnRollback(arg_33_0)
	if not store.rollback.lockmode_index then
		return
	end

	arg_33_0:openResumeDataList(store.rollback.lockmode_index)

	if system.floating_cards then
		system.floating_cards:setLocalPositionX(arg_33_0.current:getLocalPositionX())
	end
end
