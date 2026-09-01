require("/scripts/core/core.lua")
require("/scripts/suspentionpoints.lua")
require("/scripts/Store_myplay_data.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_dialog.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/app/clover_autocancel.lua")
require("/scripts/app/clover_util.lua")

sys_resumedatalist = class(gui_container)

function sys_resumedatalist.activate(arg_1_0)
	gui_container.activate(arg_1_0)
	CloverAutoCancel.Set("sys_resumedatalist", arg_1_0, "sys_resume_lockmode")

	arg_1_0.canceled = false
end

function sys_resumedatalist.openResumeDataList(arg_2_0, arg_2_1, arg_2_2)
	for iter_2_0 = 1, MAX_RESUMEDATA_NUM do
		local var_2_0 = arg_2_0.elementArray[iter_2_0]
		local var_2_1 = SuspentionPoints:getInfo(arg_2_1, iter_2_0)

		var_2_0:setInfo(var_2_1)
	end

	arg_2_0:resetCurrent(arg_2_2)
end

function sys_resumedatalist.resetCurrent(arg_3_0, arg_3_1)
	local var_3_0 = arg_3_1 or 1
	local var_3_1

	for iter_3_0 = 1, MAX_RESUMEDATA_NUM do
		if not arg_3_0.elementArray[iter_3_0]:isEmpty() and (var_3_1 == nil or math.abs(iter_3_0 - var_3_0) < math.abs(var_3_1 - var_3_0)) then
			var_3_1 = iter_3_0
		end
	end

	if var_3_1 == nil then
		var_3_1 = 1
	end

	arg_3_0.current = arg_3_0.elementArray[var_3_1]
end

function sys_resumedatalist.onElementFocus(arg_4_0, arg_4_1)
	gui_container.onElementFocus(arg_4_0, arg_4_1)

	if arg_4_1 and arg_4_1 ~= arg_4_0 then
		local var_4_0 = table_find(arg_4_0.elementArray, arg_4_1)

		arg_4_0.menu.selected_element = arg_4_1

		arg_4_0.menu:saveCursorIndex(var_4_0)

		if system.game_card then
			if arg_4_1:isNeedWarning() then
				system.game_card:showWarning()
			else
				system.game_card:hideWarning()
			end

			if CLOVER_IS_DEBUG and HOST_PLATFORM_IS_WINDOWS then
				local var_4_1 = system.game_card

				var_4_1.backup_warning:setWarningType("copy")
				var_4_1.backup_warning:activate()
			end
		end
	end
end

function sys_resumedatalist.onElementUnfocus(arg_5_0, arg_5_1)
	gui_container.onElementUnfocus(arg_5_0, arg_5_1)

	if arg_5_1 and arg_5_1 ~= arg_5_0 then
		local var_5_0 = table_find(arg_5_0.elementArray, arg_5_1)

		if system.game_card then
			system.game_card:hideWarning()
		end
	end
end

function sys_resumedatalist.onButtonClick(arg_6_0, arg_6_1)
	if arg_6_0.canceled then
		return
	end

	debugLabelPrint(string.format("LOAD SUSPENSION POINT"))

	local var_6_0 = table_find(arg_6_0.elementArray, arg_6_1)
	local var_6_1 = arg_6_1.resumeinfo

	if var_6_1 then
		local var_6_2 = CloverPadUI.pressed.btn_x

		if not var_6_2 and arg_6_0.startSound then
			arg_6_0.startSound:stop()
			arg_6_0.startSound:play()
		end

		local var_6_3 = var_6_1.game_code
		local var_6_4 = var_6_1.folder_name .. "/rollback/"
		local var_6_5 = arg_6_1:isLocked()
		local var_6_6 = var_6_1.folder_name .. "/state.time"

		CloverPadUI:EnableUserInput(false, "run")

		if var_6_2 then
			arg_6_0.menu.lockmode:onBootRollback()
			CloverElements.gametitlelist:showReplayText()
		end

		system.run_gameresume(var_6_3, var_6_4, var_6_2, var_6_5, nil, var_6_6, nil)

		if not var_6_2 then
			arg_6_1:expandAnimation()
		end
	end
end

function sys_resumedatalist.lockFloatingCard(arg_7_0, arg_7_1)
	print("-- lock card")

	local var_7_0 = arg_7_1
	local var_7_1 = system.game_card.gameinfo.game_code

	SuspentionPoints:lockSlot(var_7_1, var_7_0, true)
	arg_7_0:sramIconRefresh()
end

function sys_resumedatalist.lockFloatingCardResult(arg_8_0, arg_8_1, arg_8_2)
	print("-- lock result")

	local var_8_0 = "floating"
	local var_8_1 = arg_8_1
	local var_8_2 = system.game_card.gameinfo.game_code
	local var_8_3 = arg_8_0.elementArray[var_8_0]
	local var_8_4 = arg_8_0.elementArray[var_8_1]

	if arg_8_2 then
		print("lock success")
		SuspentionPoints:lockSlotSuccess(var_8_2, var_8_1)

		if var_8_3 then
			var_8_3:setInfo(nil)
		end

		if var_8_4 then
			var_8_4:setInfo(SuspentionPoints:getInfo(var_8_2, var_8_1))
		end

		MyplayData.Swap(var_8_2, var_8_0, var_8_1)
		system.save_setting()

		system.floating_save = nil

		system.floating_cards:setDisable()

		arg_8_0.current = arg_8_0.elementArray[arg_8_1]
	else
		print("lock fail")
	end
end

function sys_resumedatalist.deleteLock(arg_9_0, arg_9_1)
	local var_9_0 = system.game_card.gameinfo.game_code

	SuspentionPoints:deleteSlot(var_9_0, arg_9_1)
	MyplayData.Remove(var_9_0, arg_9_1)
	arg_9_0.elementArray[arg_9_1]:setInfo(nil)
end

function sys_resumedatalist.onElementCommand(arg_10_0, arg_10_1, arg_10_2)
	local var_10_0 = arg_10_0.elementArray
	local var_10_1 = table_find(var_10_0, arg_10_1)

	if var_10_1 and arg_10_2.id == GUI_COMMAND_KEYPRESS then
		if arg_10_1.resumeinfo then
			if CloverPadUI.pressed.btn_select then
				arg_10_0.menu:toChangeMode(arg_10_1, var_10_1)

				if arg_10_0.lockSound then
					arg_10_0.lockSound:stop()
					arg_10_0.lockSound:play()
				end

				return true
			end

			if CloverPadUI.pressed.down then
				arg_10_1:lockAnimation(0)
				arg_10_1:lockToggle()
				system.save_setting()

				return true
			end
		end

		if CloverPadUI.pressed.up then
			arg_10_0:Back()

			return true
		end

		if CloverPadUI.pressed.btn_start then
			arg_10_0:onButtonClick(arg_10_1)
		end

		if CloverPadUI.pressed.btn_x then
			arg_10_0:onButtonClick(arg_10_1)
		end

		if REED_DEBUG then
			if debug_store.resumedata_x_break and CloverPadDbg.pressed.btn_x then
				local var_10_2 = io.open(arg_10_1.resumeinfo.folder_name, "w")

				var_10_2:write("dead")
				var_10_2:close()
				debugLabelPrint("Break StateFile :" .. arg_10_1.resumeinfo.folder_name)
			end

			if debug_store.resumethumb_l_break and CloverPadDbg.pressed.btn_l then
				local var_10_3 = io.open(arg_10_1.resumeinfo.thumbnail, "w")

				var_10_3:write("dead")
				var_10_3:close()
				debugLabelPrint("Break ThumbnailFile :" .. arg_10_1.resumeinfo.thumbnail)
			end

			if debug_store.resumesram_r_break and CloverPadDbg.pressed.btn_r then
				local var_10_4 = io.open(titles_list[game_code].sram_file, "w")

				var_10_4:write("dead")
				var_10_4:close()
				debugLabelPrint("Break SRAM :" .. titles_list[game_code].sram_file)
			end
		end
	end

	return gui_container.onElementCommand(arg_10_0, arg_10_1, arg_10_2)
end

function sys_resumedatalist.onContainerCanceled(arg_11_0, arg_11_1)
	arg_11_0:Back()
end

function sys_resumedatalist.Back(arg_12_0)
	arg_12_0.canceled = true

	CloverAutoCancel.Set("sys_resumedatalist", nil)

	if not elementIsEnabled(arg_12_0.upElement) then
		if arg_12_0:elementIsFocusedRecursive() then
			Main:setSceneChangeCursorZ(CloverConst.Priority.FOCUS_TITLE_CURSOR_Z)
		end

		Main:toHomeMenu()

		if arg_12_0.cancelSound then
			arg_12_0.cancelSound:stop()
			arg_12_0.cancelSound:play()
		end
	else
		GUI:focusElement(arg_12_0.upElement)
	end
end

function sys_resumedatalist.showDeleteDialog(arg_13_0, arg_13_1)
	arg_13_0.dialog:showDialog(arg_13_0, function()
		arg_13_0:deleteLock(arg_13_1)
	end)

	if arg_13_0.deleteSound then
		arg_13_0.deleteSound:stop()
		arg_13_0.deleteSound:play()
	end
end

function sys_resumedatalist.isEmpty(arg_15_0)
	for iter_15_0 = 1, MAX_RESUMEDATA_NUM do
		if not arg_15_0.elementArray[iter_15_0]:isEmpty() then
			return false
		end
	end

	return true
end

function sys_resumedatalist.sramIconRefresh(arg_16_0)
	for iter_16_0, iter_16_1 in pairs(arg_16_0.elementArray) do
		iter_16_1:sramIconRefresh()
	end
end
