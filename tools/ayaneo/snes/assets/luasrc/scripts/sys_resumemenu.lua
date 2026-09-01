require("/scripts/core/core.lua")
require("/scripts/sys_resumedatalist.lua")
require("/scripts/Store_myplay_data.lua")
require("/scripts/app/clover_nodeanim.lua")
require("/scripts/app/clover_event.lua")
require("/scripts/app/clover_elements.lua")

sys_resumemenu = class(gui_container)

function sys_resumemenu.start(arg_1_0)
	arg_1_0.first_activate = false

	gui_container.start(arg_1_0)
end

function sys_resumemenu.update(arg_2_0, arg_2_1)
	gui_container.update(arg_2_0, arg_2_1)

	if arg_2_0.bg_tail then
		local var_2_0, var_2_1 = system.game_card:getWorldPosition()

		arg_2_0.bg_tail:setLocalPositionX(var_2_0)
	end
end

function sys_resumemenu.activate(arg_3_0)
	arg_3_0.current = arg_3_0.focus

	local var_3_0 = system.game_card.gameinfo.game_code
	local var_3_1 = table_unwrapNullable
	local var_3_2 = table_wrapNullable
	local var_3_3 = var_3_1(var_3_2(store).info[var_3_0].cursor)

	if var_3_3 == nil or var_3_3 < 1 or var_3_3 > MAX_RESUMEDATA_NUM then
		var_3_3 = 1
	end

	local var_3_4 = false

	if store.autoplay and store.autoplay.running then
		var_3_4 = true

		if store.autoplay.myplayIndex then
			var_3_3 = MyplayData.GetSuspentionIndex(store.autoplay.myplayIndex)
		end
	end

	arg_3_0.cardlist:openResumeDataList(var_3_0, var_3_3)
	arg_3_0.lockmode:openResumeDataList(var_3_3)

	local var_3_5 = false

	if var_3_4 == false and system.floating_save and system.floating_save.game_code == var_3_0 then
		arg_3_0.lockmode:enable()

		arg_3_0.current = arg_3_0.lockmode

		arg_3_0.emptymode:disable()
		system.floating_cards:moveToResumeList()

		if arg_3_0.cardlist:isEmpty() then
			arg_3_0.lockmode.downElement = nil
		else
			arg_3_0.lockmode.downElement = arg_3_0.cardlist
		end

		var_3_5 = true
	elseif not arg_3_0.cardlist:isEmpty() then
		arg_3_0.current = arg_3_0.cardlist

		arg_3_0.lockmode:disable()
		arg_3_0.emptymode:disable()
	else
		arg_3_0.emptymode:enable()

		arg_3_0.current = arg_3_0.emptymode
	end

	gui_container.activate(arg_3_0)

	if arg_3_0.resumemenu_position then
		arg_3_0.resumemenu_position:changeAnimation()
	end

	CloverElements.gametitlelist:cardColorDark(true)

	if arg_3_0.first_activate and system.floating_save and system.floating_save.game_code == var_3_0 and store.rollback.is_volatile == false then
		arg_3_0.lockmode:deactivate()
		arg_3_0.emptymode:disable()
		arg_3_0.cardlist:resetCurrent(var_3_3)

		arg_3_0.current = arg_3_0.cardlist

		GUI:focusElement(arg_3_0.current, true)
		arg_3_0.lockmode:onReturnRollback()
	end

	if var_3_5 then
		arg_3_0:setBGMode("overwrite")
	else
		arg_3_0:setBGMode("normal")
	end

	CloverBG.ResumeOn()
	CloverElements.resumedummy:resumeOn()
	arg_3_0:hideBackElements(true)
end

function sys_resumemenu.deactivate(arg_4_0)
	gui_container.deactivate(arg_4_0)

	local var_4_0 = system.game_card.gameinfo.game_code

	if system.floating_save and system.floating_save.game_code == var_4_0 then
		system.floating_cards:moveFromResumeList()
	end

	if arg_4_0.resumemenu_position then
		arg_4_0.resumemenu_position:revertAnimation()
	end

	CloverElements.gametitlelist:cardColorDark(nil)
	CloverBG.ResumeOff()
	CloverElements.resumedummy:resumeOff()
	arg_4_0:hideBackElements(false)
end

function sys_resumemenu.setFirstActivate(arg_5_0, arg_5_1)
	arg_5_0.first_activate = arg_5_1
end

function sys_resumemenu.getCard(arg_6_0, arg_6_1)
	if type(arg_6_1) ~= "number" or arg_6_1 < 1 or arg_6_1 > 4 then
		return nil
	end

	return arg_6_0.cardlist.elementArray[arg_6_1]
end

function sys_resumemenu.toCardList(arg_7_0)
	GUI:focusElement(arg_7_0.cardlist, true)
end

function sys_resumemenu.toLockMode(arg_8_0)
	GUI:focusElement(arg_8_0.lockmode, true)
end

function sys_resumemenu.toEmptyMode(arg_9_0)
	GUI:focusElement(arg_9_0.emptymode, true)
end

function sys_resumemenu.lockCard(arg_10_0, arg_10_1)
	arg_10_0.cardlist:lockFloatingCard(arg_10_1)
end

function sys_resumemenu.lockFloatingCardResult(arg_11_0, arg_11_1, arg_11_2)
	arg_11_0.cardlist:lockFloatingCardResult(arg_11_1, arg_11_2)

	if arg_11_2 then
		arg_11_0.lockmode:disable()
		arg_11_0.emptymode:disable()
	end
end

function sys_resumemenu.hudSelect(arg_12_0, arg_12_1)
	local var_12_0 = {
		floating = arg_12_0.hud_floating,
		locked = arg_12_0.hud_locked,
		change = arg_12_0.hud_change,
		empty = arg_12_0.hud_empty
	}

	for iter_12_0, iter_12_1 in pairs(var_12_0) do
		if iter_12_0 == arg_12_1 then
			iter_12_1:enable()
		else
			iter_12_1:disable()
		end
	end
end

function sys_resumemenu.toChangeMode(arg_13_0, arg_13_1, arg_13_2)
	arg_13_0.isSuspentionLockMode = true
	arg_13_0.changemode.card_index = arg_13_2
	arg_13_0.changemode.current = arg_13_0.changemode.elementArray[arg_13_2]

	arg_13_0.changemode:enable()

	arg_13_0.changemode.first = true

	local var_13_0 = 0.02

	if arg_13_0.changemode_position then
		arg_13_0.changemode_position:changeAnimation()

		var_13_0 = var_13_0 + arg_13_0.changemode_position.duration
	end

	tween_stop(system.tween)

	system.tween = Tween_noControl(Tween:wait(var_13_0):callback(function()
		GUI:focusElement(arg_13_0.changemode)

		arg_13_0.suspentionElement = arg_13_1

		arg_13_0.suspentionElement:activate()
		arg_13_0.suspentionElement:showOperation()
		arg_13_0.suspentionElement:changeMode()
	end)):start()
end

function sys_resumemenu.changeModeEnd(arg_15_0)
	arg_15_0.suspentionElement:hideOperation()
	arg_15_0.suspentionElement:changeModeEnd()
	arg_15_0.suspentionElement:deactivate()

	arg_15_0.suspentionElement = nil

	if arg_15_0.cardlist:isEmpty() then
		arg_15_0.lockmode.downElement = nil

		if arg_15_0.lockmode:isEnabled() then
			GUI:focusElement(arg_15_0.lockmode, true)
		else
			arg_15_0.emptymode:enable()
			GUI:focusElement(arg_15_0.emptymode, true)
		end
	else
		arg_15_0.lockmode.downElement = arg_15_0.cardlist

		GUI:focusElement(arg_15_0.cardlist, true)
	end

	arg_15_0.changemode.on_index = nil

	if arg_15_0.changemode_position then
		arg_15_0.changemode_position:revertAnimation()
	end
end

function sys_resumemenu.saveCursorIndex(arg_16_0, arg_16_1)
	local var_16_0 = system.game_card.gameinfo.game_code

	if not store.info then
		store.info = {}
	end

	if not store.info[var_16_0] then
		store.info[var_16_0] = {}
	end

	store.info[var_16_0].cursor = arg_16_1
end

function sys_resumemenu.getCursorIndex(arg_17_0)
	local var_17_0 = system.game_card.gameinfo.game_code

	return store.info[var_17_0].cursor
end

function sys_resumemenu.selectedFloatingTitle(arg_18_0)
	return
end

function sys_resumemenu.UnselectFloatingTitle(arg_19_0)
	return
end

function sys_resumemenu.sramIconRefresh(arg_20_0)
	arg_20_0.cardlist:sramIconRefresh()
end

function sys_resumemenu.playStartSound(arg_21_0)
	if arg_21_0.cardlist.startSound then
		arg_21_0.cardlist.startSound:stop()
		arg_21_0.cardlist.startSound:play()
	end
end

function sys_resumemenu.setBGMode(arg_22_0, arg_22_1)
	({
		normal = function()
			arg_22_0.overwrite_bg:disable()
			arg_22_0.normal_bg:enable()

			arg_22_0.bg_tail = arg_22_0.normal_bg:getChildByName("resumebg_tail")
		end,
		overwrite = function()
			arg_22_0.overwrite_bg:enable()
			arg_22_0.normal_bg:disable()

			arg_22_0.bg_tail = arg_22_0.overwrite_bg:getChildByName("resumebg_tail")
		end
	})[arg_22_1]()

	if arg_22_0.bg_tail then
		local var_22_0, var_22_1 = system.game_card:getWorldPosition()

		arg_22_0.bg_tail:setLocalPositionX(var_22_0)
	end
end

function sys_resumemenu.startOverwriteAnim(arg_25_0)
	OverWriteAnim.Start(arg_25_0)
end

function sys_resumemenu.hideBackElements(arg_26_0, arg_26_1)
	local var_26_0 = CloverElements.gametitlelist.menu
	local var_26_1 = {
		hud = var_26_0:getChildByName("hud"),
		menu_bar = var_26_0:getChildByName("menubar"),
		thumbnails = var_26_0:getChildByName("thumbnails")
	}

	for iter_26_0, iter_26_1 in pairs(var_26_1) do
		if arg_26_1 then
			iter_26_1:setVisible(false)
		else
			iter_26_1:setVisible(true)
		end
	end
end

function sys_resumemenu.onElementFocus(arg_27_0, arg_27_1)
	gui_container.onElementFocus(arg_27_0, arg_27_1)

	if arg_27_1 and arg_27_1 ~= arg_27_0 then
		local var_27_0 = table_find(arg_27_0.elementArray, arg_27_1)

		if arg_27_1 == arg_27_0.cardlist then
			arg_27_0:hudSelect("locked")
			arg_27_0:setBGMode("normal")
		elseif arg_27_1 == arg_27_0.lockmode then
			arg_27_0:hudSelect("floating")
			arg_27_0:setBGMode("overwrite")
		elseif arg_27_1 == arg_27_0.changemode then
			arg_27_0:hudSelect("change")
			arg_27_0:setBGMode("normal")
		elseif arg_27_1 == arg_27_0.emptymode then
			arg_27_0:hudSelect("empty")
			arg_27_0:setBGMode("normal")
		end
	end
end
