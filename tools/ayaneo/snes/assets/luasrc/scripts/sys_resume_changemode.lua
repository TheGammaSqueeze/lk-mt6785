require("/scripts/core/core.lua")
require("/scripts/suspentionpoints.lua")
require("/scripts/Store_myplay_data.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_autocancel.lua")

sys_resume_changemode = class(gui_container)

local var_0_0 = 0.5
local var_0_1 = Ease.outExpo
local var_0_2 = 10
local var_0_3 = 5

function sys_resume_changemode.activate(arg_1_0)
	gui_container.activate(arg_1_0)

	local var_1_0 = arg_1_0.menu:getCard(arg_1_0.card_index)

	var_1_0.saved_node:setZIndex(var_0_2)
	var_1_0.saved_node:setLocalPosition(-6, 0)
	var_1_0:showOperation()
	arg_1_0:trashClosed()
	arg_1_0.trash_node:enable()

	arg_1_0.nofocus = false

	CloverAutoCancel.Set("sys_resume_changemode", arg_1_0, "sys_resumedatalist")

	arg_1_0.canceled = nil
end

function sys_resume_changemode.deactivate(arg_2_0)
	gui_container.deactivate(arg_2_0)

	if arg_2_0.saved_node then
		arg_2_0.saved_node:setLocalPosition(0, 0)
	end

	arg_2_0.menu:getCard(arg_2_0.card_index):hideOperation()
	arg_2_0.trash_node:disable()
end

function sys_resume_changemode.onElementFocus(arg_3_0, arg_3_1)
	if arg_3_0.canceled then
		arg_3_0.canceled = nil

		return
	end

	gui_container.onElementFocus(arg_3_0, arg_3_1)

	if arg_3_1 and arg_3_1 ~= arg_3_0 then
		local var_3_0 = table_find(arg_3_0.elementArray, arg_3_1)

		if var_3_0 and not arg_3_0.nofocus then
			local var_3_1 = arg_3_0.menu:getCard(arg_3_0.card_index)
			local var_3_2 = arg_3_0.prev_index or var_3_0
			local var_3_3 = arg_3_0.card_index
			local var_3_4 = var_3_0
			local var_3_5 = var_3_1.saved_node

			tween_stop(var_3_5.tween)

			local var_3_6 = var_3_4 == 5 and 0.6666666666666666 or 1
			local var_3_7, var_3_8 = arg_3_0.elementArray[var_3_4]:getWorldPosition()

			var_3_5.tween = Tween:parallel(Tween:animate(var_3_5, var_0_0, "to", {
				WorldX = var_3_7,
				WorldY = var_3_8
			}, var_0_1), Tween:scaleTo(var_3_5, var_0_0, var_3_6, var_3_6, var_0_1)):start()
			arg_3_0.on_index = to_index
			arg_3_0.prev_index = var_3_0

			if var_3_4 == 5 then
				if var_3_2 ~= 5 then
					arg_3_0:trashOpen()
				end
			elseif var_3_2 == 5 then
				arg_3_0:trashClose()
			end
		end
	end

	if arg_3_0.first then
		arg_3_0.first = false
	end
end

function sys_resume_changemode.errDialogCall(arg_4_0, arg_4_1, arg_4_2)
	local var_4_0, var_4_1 = pcall(arg_4_1)

	if not var_4_0 then
		arg_4_0.err_dialog:showDialog(arg_4_0, function()
			arg_4_2()
		end)
	else
		arg_4_2()
	end

	return var_4_0, var_4_1
end

function sys_resume_changemode.onButtonClick(arg_6_0, arg_6_1)
	if arg_6_0.deleteaniation then
		return
	end

	if arg_6_0.canceled then
		arg_6_0.canceled = nil

		return
	end

	local var_6_0 = table_find(arg_6_0.elementArray, arg_6_1)

	if var_6_0 then
		if var_6_0 == 5 then
			local var_6_1 = arg_6_0.menu:getCard(arg_6_0.card_index)

			if var_6_1:isLocked() then
				debugLabelPrint("sys_resume_lockmode:onButtonClick failed")

				if arg_6_0.failureSound then
					arg_6_0.failureSound:stop()
					arg_6_0.failureSound:play()
				end

				var_6_1:showLockIcon()
			else
				arg_6_0.menu.cardlist.dialog:showDialog(arg_6_1, function()
					arg_6_0.nofocus = true

					arg_6_0:deleteAnimation(function()
						arg_6_0:errDialogCall(function()
							arg_6_0.menu.cardlist:deleteLock(arg_6_0.card_index)

							if system.floating_cards then
								system.floating_cards:sramIconRefresh()
							end
						end, function()
							arg_6_0.menu.cardlist:resetCurrent()
							arg_6_0.menu:changeModeEnd()
							arg_6_0:disable()
						end)
					end)
				end)

				if arg_6_0.successSound then
					arg_6_0.successSound:stop()
					arg_6_0.successSound:play()
				end
			end
		else
			local var_6_2 = system.game_card.gameinfo.game_code
			local var_6_3 = arg_6_0.card_index
			local var_6_4 = var_6_0

			arg_6_0:validateAnimation(arg_6_1, function()
				local var_11_0 = arg_6_0:errDialogCall(function()
					if var_6_3 ~= var_6_4 then
						SuspentionPoints:swapSlots(var_6_2, var_6_3, var_6_4)
						MyplayData.Swap(var_6_2, var_6_3, var_6_4)
						system.save_setting()
						arg_6_0.menu:getCard(var_6_3):setInfo(SuspentionPoints:getInfo(var_6_2, var_6_3))
						arg_6_0.menu:getCard(var_6_4):setInfo(SuspentionPoints:getInfo(var_6_2, var_6_4))

						arg_6_0.menu.cardlist.current = arg_6_0.menu:getCard(var_6_0)
					end
				end, function()
					arg_6_0.menu:changeModeEnd()
					arg_6_0:disable()
				end)
			end)
		end
	end
end

function sys_resume_changemode.onElementCommand(arg_14_0, arg_14_1, arg_14_2)
	if arg_14_0.deleteaniation then
		return
	end

	local var_14_0 = arg_14_0.elementArray

	if table_find(var_14_0, arg_14_1) and arg_14_2.id == GUI_COMMAND_KEYPRESS and CloverPadUI.pressed.btn_select then
		arg_14_0:back()
	end

	return gui_container.onElementCommand(arg_14_0, arg_14_1, arg_14_2)
end

function sys_resume_changemode.onContainerCanceled(arg_15_0, arg_15_1)
	if arg_15_0.deleteaniation then
		return
	end

	CloverAutoCancel.Set("sys_resume_changemode", nil)
	arg_15_0:back()
end

function sys_resume_changemode.back(arg_16_0, arg_16_1)
	arg_16_0.canceled = true

	arg_16_0:trashClosed()
	arg_16_0:cancelAnimation(arg_16_1, function()
		arg_16_0.menu:changeModeEnd()
		arg_16_0:disable()

		arg_16_0.canceled = nil
	end)
end

function sys_resume_changemode.validateAnimation(arg_18_0, arg_18_1, arg_18_2)
	local var_18_0 = table_find(arg_18_0.elementArray, arg_18_1)
	local var_18_1 = system.game_card.gameinfo.game_code

	arg_18_0.menu:getCard(arg_18_0.card_index):hideOperation()

	local var_18_2 = arg_18_0.menu:getCard(arg_18_0.card_index)
	local var_18_3 = arg_18_0.menu:getCard(var_18_0)
	local var_18_4, var_18_5 = var_18_3:getWorldPosition()
	local var_18_6, var_18_7 = var_18_2:getWorldPosition()
	local var_18_8 = var_18_2.saved_node

	tween_stop(var_18_8.tween)

	local var_18_9 = 80

	tween_stop(system.tween)

	system.tween = Tween_noControl(Tween:sequence(Tween_worldTo(var_18_2.saved_node, 0, var_18_4, var_18_5, Ease.outSine), Tween:callback(function()
		var_18_2.saved_node:setLocalScale(1.1, 1.1)
		var_18_2:changeModeEnd()
	end), Tween:wait(0.02), Tween:scaleTo(var_18_2.saved_node, 0.05, 1, 1, Ease.outSine)), Tween_if(arg_18_0.card_index ~= var_18_0 and not var_18_3:isEmpty(), Tween:callback(function()
		var_18_3.saved_node:setZIndex(var_0_3)
	end), Tween_worldTo(var_18_3.saved_node, 0.01, var_18_4, var_18_5 - var_18_9), Tween:wait(0.02), Tween_worldTo(var_18_3.saved_node, 0.1, var_18_6, var_18_7 - var_18_9), Tween:wait(0.02), Tween_worldTo(var_18_3.saved_node, 0.05, var_18_6, var_18_7), Tween:callback(function()
		var_18_3.saved_node:setLocalScale(1.1, 1.1)
		var_18_3:changeModeEnd()
	end), Tween:wait(0.02), Tween:scaleTo(var_18_3.saved_node, 0.05, 1, 1, Ease.outSine)), Tween:wait(0.1), Tween:callback(function()
		for iter_22_0 = 1, MAX_RESUMEDATA_NUM do
			local var_22_0 = arg_18_0.menu:getCard(iter_22_0).saved_node

			tween_stop(var_22_0.tween)
			var_22_0:setLocalPosition(0, 0)
			var_22_0:setLocalScale(1, 1)
			var_22_0:setZIndex(0)
		end

		system.cursor:cursorHide()

		if arg_18_2 then
			arg_18_2()
		end
	end)):start()
end

function sys_resume_changemode.cancelAnimation(arg_23_0, arg_23_1, arg_23_2)
	if arg_23_0.cancelSound then
		arg_23_0.cancelSound:stop()
		arg_23_0.cancelSound:play()
	end

	local var_23_0 = arg_23_0.menu:getCard(arg_23_0.card_index)

	var_23_0:hideOperation()

	local var_23_1 = 0.05
	local var_23_2 = var_23_0.saved_node

	tween_stop(var_23_2.tween)
	tween_stop(system.tween)

	system.tween = Tween_noControl(Tween:parallel(Tween:moveTo(var_23_2, var_23_1, 0, 0), Tween:scaleTo(var_23_2, var_23_1, 1, 1)), Tween:wait(0.05), Tween:callback(function()
		var_23_2:setZIndex(0)
		arg_23_2()
	end)):start()
end

function sys_resume_changemode.deleteAnimation(arg_25_0, arg_25_1)
	arg_25_0.deleteaniation = true

	local var_25_0 = arg_25_0.trash_pos_node or arg_25_0.trash_node
	local var_25_1 = arg_25_0.menu:getCard(arg_25_0.card_index)
	local var_25_2 = var_25_1.saved_node
	local var_25_3, var_25_4 = var_25_0:getWorldPosition()

	arg_25_0.trash_node:setZIndex(30)
	arg_25_0.menu:getCard(arg_25_0.card_index):hideOperation()
	tween_stop(var_25_2.tween)
	tween_stop(system.tween)

	system.tween = Tween_noControl(Tween:sequence(Tween:parallel(Tween:scaleTo(var_25_2, 0.2, 0.2, 0.2, Ease.inSine), Tween_worldTo(var_25_2, 0.2, var_25_3, var_25_4, Ease.inSine)), Tween:callback(function()
		arg_25_0:trashIn()
		var_25_2:disable()
		arg_25_0.trash_node:setLocalScale(1.2, 1.2)
	end), Tween:wait(0.05), Tween:scaleTo(arg_25_0.trash_node, 0.05, 1, 1), Tween:wait(0.8), Tween:callback(function()
		var_25_1:changeModeEnd()
		var_25_2:setLocalPosition(0, 0)
		var_25_2:setLocalScale(1, 1)
		var_25_2:setZIndex(0)
		var_25_2:enable()
		arg_25_0.trash_node:setZIndex(0)

		arg_25_0.deleteaniation = false

		system.cursor:cursorHide()
		arg_25_1()
	end))):start()
end

function sys_resume_changemode.trashClosed(arg_28_0)
	local var_28_0 = table_wrapNullable

	var_28_0(arg_28_0).trash_open:disable()
	var_28_0(arg_28_0).trash_close:disable()
	var_28_0(arg_28_0).trash_in:disable()
	var_28_0(arg_28_0).trash_inactive:enable()
	var_28_0(arg_28_0).trash_in_sound:disable()
end

function sys_resume_changemode.trashOpen(arg_29_0)
	local var_29_0 = table_wrapNullable

	var_29_0(arg_29_0).trash_inactive:disable()
	var_29_0(arg_29_0).trash_close:disable()
	var_29_0(arg_29_0).trash_in:disable()
	var_29_0(arg_29_0).trash_open:enable()
	var_29_0(arg_29_0).trash_open:restart()
end

function sys_resume_changemode.trashClose(arg_30_0)
	local var_30_0 = table_wrapNullable

	var_30_0(arg_30_0).trash_inactive:disable()
	var_30_0(arg_30_0).trash_in:disable()
	var_30_0(arg_30_0).trash_open:disable()
	var_30_0(arg_30_0).trash_close:enable()
	var_30_0(arg_30_0).trash_close:restart()
end

function sys_resume_changemode.trashIn(arg_31_0)
	local var_31_0 = table_wrapNullable

	var_31_0(arg_31_0).trash_inactive:disable()
	var_31_0(arg_31_0).trash_open:disable()
	var_31_0(arg_31_0).trash_close:disable()
	var_31_0(arg_31_0).trash_in:enable()
	var_31_0(arg_31_0).trash_in:restart()
	var_31_0(arg_31_0).trash_in_sound:enable()
	var_31_0(arg_31_0).trash_in_sound:play()
end
