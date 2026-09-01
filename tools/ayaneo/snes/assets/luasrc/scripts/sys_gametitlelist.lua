require("/scripts/core/core.lua")
require("/scripts/helper_index.lua")
require("/scripts/helper_table.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_layout.lua")
require("/scripts/app/clover_bg.lua")
require("/scripts/app/clover_elements.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/app/clover_event.lua")
require("/scripts/app/clover_task.lua")
require("/scripts/app/clover_dialog.lua")
require("/scripts/app/clover_util.lua")
require("/scripts/app/clover_autocancel.lua")

sys_gametitlelist = class(gui_container)
GAME_CODE_STARFOX_J = "CLV-P-VADKJ"
GAME_CODE_STARFOX_E = "CLV-P-SADKE"

local var_0_0 = {
	is_running = false,
	event = CloverEvent.new(),
	arg = {},
	listener = CloverDialogListener.new(nil),
	variables = {}
}

function sys_gametitlelist.start(arg_1_0)
	local var_1_0 = {}
	local var_1_1 = 0

	for iter_1_0, iter_1_1 in pairs(GameList.order) do
		local var_1_2 = arg_1_0.card:instantiate()

		arg_1_0.elements:addChildNode(var_1_2)
		table.insert(var_1_0, var_1_2)
		var_1_2:setInfo(iter_1_1)

		var_1_2.game_index = var_1_1

		if iter_1_1.game_code == store.current_game_code then
			arg_1_0.focus = var_1_2
		end

		var_1_1 = var_1_1 + 1
	end

	arg_1_0.elementArray = var_1_0
	arg_1_0.titleNum = var_1_1
	arg_1_0.sortruleIndex = store.sortrule or 1

	if store.autoplay and store.autoplay.running then
		arg_1_0.sortruleIndex = 1
	end

	arg_1_0.sortrulePrevIndex = arg_1_0.sortruleIndex

	local var_1_3, var_1_4 = arg_1_0.narabe1:getWorldPosition()
	local var_1_5, var_1_6 = arg_1_0.narabe2:getWorldPosition()
	local var_1_7 = var_1_5 - var_1_3
	local var_1_8 = var_1_6 - var_1_4

	arg_1_0.HGap, arg_1_0.VGap = var_1_7, var_1_8

	arg_1_0.elements:setWorldPosition(var_1_3, var_1_4)

	function createCursorComponent(arg_2_0)
		local var_2_0 = WorldNode:create()
		local var_2_1 = SpriteComponent:create()

		var_2_0.cursor_component = var_2_1

		local var_2_2, var_2_3 = var_1_0[1].cursor_component:getSize()
		local var_2_4 = var_1_0[1].cursor_component:getNode():getLocalPositionY()

		var_2_1:setSize(var_2_2, var_2_3)
		var_2_1:disable()
		var_2_0:setLocalPosition(arg_2_0, var_2_4)
		var_2_0:attachComponent(var_2_1)
		arg_1_0.cursor_elements:addChildNode(var_2_0)
		arg_1_0.cursor_elements:enable()

		return var_2_1
	end

	arg_1_0.cursorComponentArray = {}

	table.insert(arg_1_0.cursorComponentArray, createCursorComponent(arg_1_0.HGap * -1.5))
	table.insert(arg_1_0.cursorComponentArray, createCursorComponent(arg_1_0.HGap * 1.5))
	gui_container.start(arg_1_0)

	arg_1_0.layout.orgX = 0
	arg_1_0.layout.orgY = 0
	arg_1_0.firstIndex = nil
	arg_1_0.tweenTime = 0
	arg_1_0.const = {
		loopSpace = 0,
		clippingSpaceScale = 1.5
	}
	arg_1_0.showSortInfo = false
	arg_1_0.scrollSpeed = 0

	DbgMenu:entry("sys_gametitle_list", arg_1_0)

	arg_1_0.menubarL = arg_1_0.downElement

	CloverLayout.InitTitlePos(arg_1_0.HGap, 4)
end

function sys_gametitlelist.update(arg_3_0, arg_3_1)
	gui_container.update(arg_3_0, arg_3_1)
	arg_3_0:updateScrollPosition(arg_3_1)

	-- Safeguard against a very fast scroll interrupting the selected card's active fade and
	-- leaving it in the corrupted "blank" end-state: active node on, inactive off, but the box
	-- art + icons at alpha 0. This exact state never occurs mid-fade (inactive stays enabled
	-- until the fade completes), so snapping it fully active here does not disturb the normal
	-- single-move fade.
	local var_3_2 = system.game_card
	if var_3_2 and var_3_2.active_node and var_3_2.inactive_node
		and var_3_2.active_node:isEnabled() and not var_3_2.inactive_node:isEnabled()
		and var_3_2.active_node.card:getAlpha() < 0.02 then
		var_3_2:enableActiveComponent(0)
	end

	if REED_DEBUG and (CloverPadDbg.release.btn_l or CloverPadDbg.release.btn_r) then
		arg_3_0.current:setDebugLabel("")
	end

	arg_3_0:unselected_title()
end

function sys_gametitlelist.updateLoopDisplay(arg_4_0, arg_4_1)
	local var_4_0 = arg_4_0.HGap or 0
	local var_4_1 = arg_4_0.elements:getLocalPositionX()
	local var_4_2 = 1 + math.floor(-var_4_1 / var_4_0)
	local var_4_3 = #arg_4_0.layout.nodesIndexes
	local var_4_4 = {}
	local var_4_5 = arg_4_0.const.loopSpace
	local var_4_6 = arg_4_0.layout.nodesIndexes[1].posX

	for iter_4_0 = 1, var_4_3 do
		local var_4_7 = var_4_6 + var_4_0 * (iter_4_0 - 1)
		local var_4_8 = iter_4_0 - var_4_2

		if var_4_8 > 8 then
			var_4_7 = var_4_7 - var_4_0 * var_4_3 - var_4_5
		elseif var_4_8 < -8 then
			var_4_7 = var_4_7 + var_4_0 * var_4_3 + var_4_5
		end

		var_4_4[iter_4_0] = var_4_7
	end

	for iter_4_1 = 1, var_4_3 do
		arg_4_0.elementArray[iter_4_1]:setLocalPositionX(var_4_4[iter_4_1])
	end
end

function sys_gametitlelist.updateFocus(arg_5_0)
	arg_5_0:onElementFocus(arg_5_0.current)
end

function sys_gametitlelist.activate(arg_6_0)
	for iter_6_0, iter_6_1 in pairs(arg_6_0.elementArray) do
		iter_6_1:disableActiveComponent()
	end

	gui_container.activate(arg_6_0)

	if arg_6_0.menu then
		arg_6_0.menu:hudSelect("gametitle")
	end

	CloverAutoCancel.ClearContinuous()
end

function sys_gametitlelist.deactivate(arg_7_0)
	gui_container.deactivate(arg_7_0)
	-- CLOVER keeps a deactivated container's visual subtree rendered; deactivate only gates
	-- input/logic and input is focus-driven. Re-enable rendering so the game carousel stays
	-- visible behind the menubar HUD, matching the reference.
	Host.setEnabled(arg_7_0, true)
end

function sys_gametitlelist.onElementFocus(arg_8_0, arg_8_1)
	local var_8_0 = arg_8_0.current

	gui_container.onElementFocus(arg_8_0, arg_8_1)

	if arg_8_1 and arg_8_1 ~= arg_8_0 then
		local var_8_1 = table_find(arg_8_0.elementArray, arg_8_1)
		local var_8_2
		local var_8_3 = arg_8_1.gameinfo
		local var_8_4 = var_8_3.game_code

		-- Snap the game title immediately, as the decompiled source does. A hi-fps device capture
		-- (minimotion, title band) shows the title updates on the SAME frame the box-art slide begins
		-- (0ms relative to the slide onset, measured twice). An earlier revision deferred it ~0.1s to
		-- match a compressed-video absolute time of ~127ms, but that was device input latency, not a
		-- real delay, and it left the web caption ~100ms behind its own (low-latency) slide.
		-- The authored GameTitle colour is a mid grey (0.298); the reference renders it solid black.
		arg_8_0.titleLabel:setText(var_8_3.raw_title)
		arg_8_0.titleLabel:setColor(0, 0, 0, 1)

		store.current_game_code = var_8_3.game_code
		system.game_card = arg_8_1

		-- Cross-fade the selected card's blue frame over 0.2s while scrolling, matching the
		-- decompiled source: the card you leave and the one you land on both fade over the same
		-- 0.2s. This works now that the tween start-value getters exist (getLocalScaleX/Y,
		-- getAlpha): before, the card's scaleTo captured a nil start and blanked the whole card
		-- for the fade duration, which read as flicker. A slower 0.5s fade-out was tried but it
		-- left a trail of blue frames still fading out on the cards behind you during a fast
		-- scroll and read as the highlight fading out then back in on every step; the hardware
		-- does neither, so keep the fade symmetric and short.
		local var_8_5

		if arg_8_0.tweenTime and arg_8_0.tweenTime > 0 then
			var_8_5 = 0.2
		end

		if var_8_0 and var_8_0 ~= arg_8_1 then
			var_8_0:disableActiveComponent(var_8_5)
		end

		if arg_8_0:isSelectable(arg_8_1.gameinfo.game_code) then
			arg_8_1:enableActiveComponent(var_8_5)
			arg_8_0:unlock_message_popup(nil)
			arg_8_1:unlock_shake_cancel()
		end

		arg_8_1:refreshInfo()

		if arg_8_0.floating_cards and system.floating_save then
			if var_8_4 == system.floating_save.game_code then
				arg_8_0.floating_cards:setSelected()
				CloverElements.resumedummy:selectTitle()
				CloverElements.resumemenu:selectedFloatingTitle()
			else
				arg_8_0.floating_cards:setUnselected()
				CloverElements.resumedummy:unselectTitle()
				CloverElements.resumemenu:UnselectFloatingTitle()
			end
		end

		if arg_8_0.thumbnail_node then
			arg_8_0.thumbnail_node:setCurrentIndex(var_8_1, arg_8_0.tweenTime)
		end

		local var_8_6 = false
		local var_8_7 = false
		local var_8_8 = arg_8_0.sortrulePrevIndex ~= arg_8_0.sortruleIndex

		arg_8_0.sortrulePrevIndex = arg_8_0.sortruleIndex

		local var_8_9, var_8_10 = arg_8_1:getLocalPosition()

		if var_8_1 then
			var_8_9 = arg_8_0.layout.nodesIndexes[var_8_1].posX
			var_8_10 = arg_8_0.layout.nodesIndexes[var_8_1].posY
		end

		local var_8_11 = arg_8_0.layout.orgX - var_8_9
		local var_8_12 = arg_8_0.layout.orgY - var_8_10
		local var_8_13 = arg_8_0.elements:getLocalPositionX()
		local var_8_14 = var_8_13

		if arg_8_0.tweenX then
			var_8_14 = arg_8_0.tweenX
		end

		local var_8_15 = arg_8_0.HGap
		local var_8_16 = #arg_8_0.layout.nodesIndexes
		local var_8_17 = math.floor(var_8_16 / 2)

		function calcClipping(arg_9_0, arg_9_1)
			local var_9_0 = arg_8_0.layout.orgX - arg_8_0.layout.nodesIndexes[1].posX - arg_9_0

			if var_9_0 < arg_9_1 then
				arg_9_0 = arg_9_0 + (var_9_0 - arg_9_1)
			end

			local var_9_1 = arg_8_0.layout.orgX - arg_8_0.layout.nodesIndexes[var_8_16].posX - arg_9_0

			if var_9_1 > -arg_9_1 then
				arg_9_0 = arg_9_0 + (var_9_1 + arg_9_1)
			end

			return arg_9_0
		end

		if arg_8_0.const.centering then
			var_8_11 = calcClipping(var_8_11, arg_8_0.const.clippingSpaceScale * var_8_15)
		end

		if var_8_1 < var_8_17 / 2 then
			if var_8_13 < -var_8_17 * var_8_15 then
				var_8_13 = var_8_13 + var_8_16 * var_8_15
				var_8_14 = var_8_14 + var_8_16 * var_8_15
			end
		elseif var_8_1 > var_8_16 - var_8_17 / 2 and var_8_13 > -var_8_17 * var_8_15 then
			var_8_13 = var_8_13 - var_8_16 * var_8_15
			var_8_14 = var_8_14 - var_8_16 * var_8_15
		end

		if not arg_8_0.const.centering then
			local var_8_18 = var_8_11 - var_8_14
			local var_8_19 = var_8_15 * arg_8_0.const.clippingSpaceScale

			if arg_8_0.firstIndex == nil or var_8_8 or arg_8_0.noScroll then
				if arg_8_0.firstIndex == nil and store.gametitlelistDX then
					var_8_19 = -store.gametitlelistDX
				end

				var_8_11 = var_8_11 - var_8_19
				store.gametitlelistDX = -var_8_19
			elseif var_8_18 > 0 and var_8_19 < var_8_18 then
				var_8_11 = var_8_11 - var_8_19
				store.gametitlelistDX = -var_8_19
			elseif var_8_18 < 0 and var_8_18 < -var_8_19 then
				var_8_11 = var_8_11 + var_8_19
				store.gametitlelistDX = var_8_19
			else
				var_8_11 = var_8_11 - var_8_18
				store.gametitlelistDX = -var_8_18
			end
		end

		if arg_8_0.firstIndex == nil then
			var_8_13 = var_8_11
			arg_8_0.firstIndex = var_8_1
			arg_8_0.tweenTime = 0
		end

		if var_8_8 or arg_8_0.noScroll then
			var_8_13 = var_8_11
			var_8_7 = true
			arg_8_0.noScroll = false
			arg_8_0.tweenTime = 0
		end

		tween_stop(arg_8_0.tween)
		arg_8_0.elements:setLocalPositionX(var_8_13)

		local var_8_20, var_8_21 = arg_8_0.elements:getLocalPosition()
		local var_8_22 = arg_8_0.tweenTime

		if math.abs(var_8_11 - var_8_13) < arg_8_0.HGap then
			var_8_22 = arg_8_0.tweenTime * math.abs(var_8_11 - var_8_13) / arg_8_0.HGap
		end

		arg_8_0.tween = Tween:sequence(Tween:moveTo(arg_8_0.elements, var_8_22, var_8_11, var_8_21, Ease.linear), Tween:callback(function()
			-- Re-home the cursor onto the focused card once the strip finishes centering it.
			-- onElementFocus fires only on focus change and, while a card is still scrolling in,
			-- parks the cursor on a static edge anchor; under fast repeated input the cursor is
			-- then left stranded on empty space when scrolling stops. Guard on the carousel still
			-- being shown so this does not fire after leaving to the menubar or a game. Also force
			-- the settled card fully active: fast scroll can interrupt its alpha fade and leave the
			-- box art + icons at alpha 0 (blank selected card), which enableActiveComponent now heals.
			if arg_8_0.elements:isEnabled() and system.game_card and system.game_card.cursor_component then
				system.cursor:setRounded(system.game_card.cursor_component, false)
			end
		end)):start()
		arg_8_0.tweenX = var_8_11

		if math.abs(var_8_13 - var_8_11) > 1 then
			if var_8_11 < var_8_13 then
				CloverBG.ScrollRight()
			else
				CloverBG.ScrollLeft()
			end
		end

		if arg_8_0.next_cursor_z then
			system.cursor:setNextStartZ(arg_8_0.next_cursor_z)
		end

		arg_8_0:updateLoopDisplay(true)

		if var_8_7 then
			arg_8_1:setCursor()
		else
			local var_8_23, var_8_24 = arg_8_1:getWorldPosition()


			if var_8_23 >= -arg_8_0.HGap * 1.5 and var_8_23 <= arg_8_0.HGap * 1.5 then
				system.cursor:setRounded(arg_8_1.cursor_component, false)
			elseif var_8_23 < -arg_8_0.HGap * 1.5 then
				system.cursor:setRounded(arg_8_0.cursorComponentArray[1], false)
			elseif var_8_23 > arg_8_0.HGap * 1.5 then
				system.cursor:setRounded(arg_8_0.cursorComponentArray[2], false)
			end
		end

		if arg_8_0.next_cursor_z then
			arg_8_0.next_cursor_z = nil

			system.cursor:setNextStartZ(0)
		end

		arg_8_0.tweenTime = 0
	end
end

function sys_gametitlelist.updateScrollPosition(arg_10_0, arg_10_1)
	local var_10_0

	arg_10_0:updateLoopDisplay(var_10_0)
end

function sys_gametitlelist.onButtonClick(arg_11_0, arg_11_1)
	if arg_11_1.startSound then
		arg_11_1.startSound:stop()
		arg_11_1.startSound:play()
	end

	if arg_11_0:isSelectable(arg_11_1.gameinfo.game_code) == nil then
		if arg_11_1.startSound then
			arg_11_1.startSound:stop()
		end

		if arg_11_1.bouncedSound then
			arg_11_1.bouncedSound:stop()
			arg_11_1.bouncedSound:play()
		end

		arg_11_1:unlock_shake()
		arg_11_0:unlock_message_popup(true)

		return
	end

	if table_find(arg_11_0.elementArray, arg_11_1) then
		-- Launching a game needs the emulator, which is out of scope. The original flow
		-- (EnableUserInput(false, "run") + run_gamestart + expandAnimation) never returns
		-- without an emulator and leaves the menu with no focus and a stuck input lock, so
		-- the d-pad dies. Skip it: pressing A just plays the start sound and stays on the menu.
		local var_11_0 = arg_11_1.gameinfo
	end
end

function sys_gametitlelist.onElementCommand(arg_12_0, arg_12_1, arg_12_2)
	if arg_12_0:forwardCommandToPreListener(arg_12_2) then
		return true
	end

	local var_12_0 = arg_12_0.elementArray
	local var_12_1 = arg_12_0.fixed_node_num or #var_12_0
	local var_12_2 = table_find(var_12_0, arg_12_1)

	if var_12_2 and arg_12_2.id == GUI_COMMAND_KEYPRESS then
		if CloverPadUI.pressed.btn_select then
			arg_12_0:unlock_message_popup(nil)
			arg_12_1:unlock_shake_cancel()

			arg_12_0.sortruleIndex = index_loop_next(arg_12_0.sortruleIndex, GameList:getSortRuleMax())

			arg_12_0:setSort(arg_12_0.sortruleIndex)

			store.sortrule = arg_12_0.sortruleIndex

			if arg_12_0.sortSound then
				arg_12_0.sortSound:stop()
				arg_12_0.sortSound:play()
			end

			return true
		end

		if CloverPadUI.pressed.btn_start then
			if arg_12_0:isSelectable(arg_12_1.gameinfo.game_code) then
				arg_12_0:onButtonClick(arg_12_1)

				return true
			else
				if arg_12_1.bouncedSound then
					arg_12_1.bouncedSound:stop()
					arg_12_1.bouncedSound:play()
				end

				arg_12_1:unlock_shake()
				arg_12_0:unlock_message_popup(true)
			end
		end

		if CloverPadUI.pressed.down then
			arg_12_0:unlock_message_popup(nil)
			arg_12_1:unlock_shake_cancel()
			Main:toResumeMenu()

			if arg_12_0.resumelistSound then
				arg_12_0.resumelistSound:stop()
				arg_12_0.resumelistSound:play()
			end

			return true
		end

		if CloverPadUI.pressed.left then
			if GUI.state == 1 then
				arg_12_0.scrollSpeed = arg_12_0.HGap / GUI.H.REPEAT_DELAY
				arg_12_0.tweenTime = GUI.H.REPEAT_DELAY
			elseif GUI.state == 2 then
				arg_12_0.scrollSpeed = arg_12_0.HGap / GUI.H.REPEAT_RATE
				arg_12_0.tweenTime = GUI.H.REPEAT_RATE
			else
				arg_12_0.tweenTime = GUI.H.REPEAT_RATE
			end

			local var_12_3 = var_12_2

			repeat
				var_12_3 = index_loop_prev(var_12_3, var_12_1)
			until elementIsEnabled(var_12_0[var_12_3]) or var_12_3 == var_12_2

			var_12_2 = var_12_3

			GUI:focusElement(var_12_0[var_12_2])

			return true
		elseif CloverPadUI.pressed.right then
			if GUI.state == 1 then
				arg_12_0.scrollSpeed = arg_12_0.HGap / GUI.H.REPEAT_DELAY
				arg_12_0.tweenTime = GUI.H.REPEAT_DELAY
			elseif GUI.state == 2 then
				arg_12_0.scrollSpeed = arg_12_0.HGap / GUI.H.REPEAT_RATE
				arg_12_0.tweenTime = GUI.H.REPEAT_RATE
			else
				arg_12_0.tweenTime = GUI.H.REPEAT_RATE
			end

			local var_12_4 = var_12_2

			repeat
				var_12_4 = index_loop_next(var_12_4, var_12_1)
			until elementIsEnabled(var_12_0[var_12_4]) or var_12_4 == var_12_2

			local var_12_5 = var_12_4

			GUI:focusElement(var_12_0[var_12_5])

			return true
		elseif CloverPadUI.pressed.up and elementIsEnabled(arg_12_0.upElement) then
			arg_12_0:unlock_message_popup(nil)
			arg_12_1:unlock_shake_cancel()
			GUI:focusElement(arg_12_0.upElement)
			arg_12_0:deactivate()

			return true
		end

		if CloverPadUI:isCancelPressed() and system.floating_save and arg_12_0.onContainerCanceled then
			local var_12_6 = arg_12_0:onContainerCanceled(arg_12_1)
			local var_12_7 = table_find_if(arg_12_0.elementArray, function(arg_13_0)
				return arg_13_0.gameinfo.game_code == system.floating_save.game_code
			end)

			arg_12_0.noScroll = true

			GUI:focusElement(nil)
			GUI:focusElement(var_12_0[var_12_7])

			if var_12_6 == nil then
				return true
			else
				return var_12_6
			end
		end
	end

	return false
end

function sys_gametitlelist.onContainerCanceled(arg_14_0, arg_14_1)
	arg_14_0.cancelPressed = true
end

function sys_gametitlelist.setSort(arg_15_0, arg_15_1, arg_15_2, arg_15_3)
	local var_15_0 = arg_15_1 or 1
	local var_15_1, var_15_2, var_15_3 = GameList:sortOrder(var_15_0)

	for iter_15_0, iter_15_1 in ipairs(arg_15_0.elementArray) do
		local var_15_4 = var_15_1[iter_15_0]

		iter_15_1:setInfo(var_15_4)

		if debug_store.debugShowSortInfo then
			iter_15_1:setDebugLabel(var_15_3(var_15_4))
		end
	end

	if not arg_15_2 then
		arg_15_0.sort_order:enable()
		arg_15_0.sort_order:showSortOrder(var_15_2)
	end

	if arg_15_0.thumbnail_node then
		arg_15_0.thumbnail_node:setOrder(var_15_1)
	end

	local var_15_5 = arg_15_0.elementArray[1]

	if arg_15_3 then
		local var_15_6 = table_find_if(arg_15_0.elementArray, function(arg_16_0)
			return arg_16_0.gameinfo.game_code == arg_15_3
		end)

		if var_15_6 then
			var_15_5 = arg_15_0.elementArray[var_15_6]
		end
	end

	for iter_15_2, iter_15_3 in pairs(arg_15_0.elementArray) do
		if iter_15_3 ~= var_15_5 then
			iter_15_3:disableActiveComponent()
		end
	end

	print("先頭のカードをフォーカス")

	arg_15_0.noScroll = true
	arg_15_0.current = var_15_5

	GUI:focusElement(nil)
	GUI:focusElement(var_15_5, true)
end

function sys_gametitlelist.getStarFoxGameCode(arg_17_0)
	local var_17_0

	if system.is_nes() then
		var_17_0 = GAME_CODE_STARFOX_E
	else
		var_17_0 = GAME_CODE_STARFOX_J
	end

	return var_17_0
end

function sys_gametitlelist.isSelectable(arg_18_0, arg_18_1)
	if (arg_18_1 == GAME_CODE_STARFOX_J or arg_18_1 == GAME_CODE_STARFOX_E) and system.isLockStarfox2() then
		return nil
	end

	return true
end

function sys_gametitlelist.unselected_title(arg_19_0)
	if system.isLockStarfox2() then
		arg_19_0:cardlist_unlock_state_reset()

		local var_19_0 = arg_19_0:getStarFoxGameCode()

		if var_19_0 then
			local var_19_1 = table_find_if(arg_19_0.elementArray, function(arg_20_0)
				return arg_20_0.gameinfo.game_code == var_19_0
			end)

			if var_19_1 then
				arg_19_0.elementArray[var_19_1]:getChildByName("lock"):enable()

				if arg_19_0.thumbnail_node then
					arg_19_0.thumbnail_node:setUnselected(var_19_1)
				end
			end
		end
	elseif CLOVER_IS_DEBUG and CloverPadUI.down.btn_r and CloverPadUI.pressed.btn_x then
		system.lockStarfox2()

		local var_19_2 = arg_19_0:getStarFoxGameCode()

		if var_19_2 then
			local var_19_3 = table_find_if(arg_19_0.elementArray, function(arg_21_0)
				return arg_21_0.gameinfo.game_code == var_19_2
			end)

			if var_19_3 then
				local var_19_4 = arg_19_0.elementArray[var_19_3]

				var_19_4.lock:enable()
				var_19_4:getChildByName("unlock"):disable()
			end
		end
	end
end

function sys_gametitlelist.cardlist_unlock_state_reset(arg_22_0)
	local var_22_0 = #arg_22_0.layout.nodesIndexes

	for iter_22_0 = 1, var_22_0 do
		element = arg_22_0.elementArray[iter_22_0]

		if arg_22_0:isSelectable(element.gameinfo.game_code) then
			element:getChildByName("lock"):disable()
		end
	end
end

function sys_gametitlelist.unlock_message_popup(arg_23_0, arg_23_1)
	if system.isStarfoxClear() then
		arg_23_1 = nil
	end

	if not system.isLockStarfox2() then
		arg_23_1 = nil
	end

	local var_23_0 = arg_23_0:getStarFoxGameCode()

	if var_23_0 then
		local var_23_1 = table_find_if(arg_23_0.elementArray, function(arg_24_0)
			return arg_24_0.gameinfo.game_code == var_23_0
		end)

		if var_23_1 then
			local var_23_2 = arg_23_0.elementArray[var_23_1]

			if arg_23_1 then
				arg_23_0.popup_tween = Tween:sequence(Tween:wait(0.1), Tween:callback(function()
					var_23_2.unlock_message:enable()
					var_23_2.unlock_message:setLocalScale(0.5, 0.5)
				end), Tween:scaleTo(var_23_2.unlock_message, CloverConst.Suspension.POPUP_SCALING_SECONDS, 1, 1)):start()
			else
				tween_stop(arg_23_0.popup_tween)
				var_23_2.unlock_message:disable()
			end
		end
	end
end

function sys_gametitlelist.setCursorOnStarfox2(arg_26_0)
	local var_26_0

	for iter_26_0, iter_26_1 in ipairs(arg_26_0.elementArray) do
		if iter_26_1 == arg_26_0.current then
			var_26_0 = iter_26_0
		end
	end

	local var_26_1 = arg_26_0:getStarFoxGameCode()

	if var_26_1 then
		local var_26_2 = table_find_if(arg_26_0.elementArray, function(arg_27_0)
			return arg_27_0.gameinfo.game_code == var_26_1
		end)

		if var_26_2 then
			indexB = var_26_0 - var_26_2

			if indexB > 0 then
				if indexB < #arg_26_0.elementArray / 2 then
					rotate_dir = true
				else
					rotate_dir = false
				end
			else
				indexB = 0 - indexB

				if indexB < #arg_26_0.elementArray / 2 then
					rotate_dir = false
				else
					rotate_dir = true
				end
			end

			local var_26_3 = arg_26_0.elementArray[var_26_2]

			if arg_26_0:setCursorOnCard(var_26_3, rotate_dir) then
				var_26_3:setCursor()

				return true
			end
		end
	end

	return false
end

function sys_gametitlelist.setCursorOnCard(arg_28_0, arg_28_1, arg_28_2)
	local var_28_0 = arg_28_0.elementArray
	local var_28_1 = arg_28_0.fixed_node_num or #var_28_0
	local var_28_2 = table_find(var_28_0, arg_28_1)

	if var_28_2 then
		if arg_28_2 then
			arg_28_0.scrollSpeed = arg_28_0.HGap / GUI.H.REPEAT_RATE * 3
			arg_28_0.tweenTime = GUI.H.REPEAT_RATE * 3

			local var_28_3 = var_28_2

			repeat
				var_28_3 = index_loop_prev(var_28_3, var_28_1)
			until var_28_3 == var_28_2

			var_28_2 = var_28_3

			GUI:focusElement(var_28_0[var_28_2])

			return true
		else
			arg_28_0.scrollSpeed = arg_28_0.HGap / GUI.H.REPEAT_RATE * 3
			arg_28_0.tweenTime = GUI.H.REPEAT_RATE * 3

			local var_28_4 = var_28_2

			repeat
				var_28_4 = index_loop_next(var_28_4, var_28_1)
			until var_28_4 == var_28_2

			local var_28_5 = var_28_4

			GUI:focusElement(var_28_0[var_28_5])

			return true
		end
	end

	return false
end

function sys_gametitlelist.getLockedCard(arg_29_0)
	-- Main:start wires the StarFox 2 unlock event before this list has built its
	-- elementArray, and our game list has no locked title anyway; return nil safely.
	if not arg_29_0.elementArray then
		return nil
	end

	local var_29_0 = arg_29_0:getStarFoxGameCode()
	local var_29_1 = table_find_if(arg_29_0.elementArray, function(arg_30_0)
		return arg_30_0.gameinfo.game_code == var_29_0
	end)

	return var_29_1 and arg_29_0.elementArray[var_29_1] or nil
end

function sys_gametitlelist.isUnselectedTitle(arg_31_0, arg_31_1)
	local var_31_0 = arg_31_0:getStarFoxGameCode()

	if table_find_if(arg_31_0.elementArray, function(arg_32_0)
		return arg_32_0.gameinfo.game_code == var_31_0
	end) == arg_31_1 and arg_31_0:getLockedCard().lock:isEnabled() then
		return true
	end

	return false
end

function sys_gametitlelist.setUnlockEvent(arg_33_0, arg_33_1)
	local var_33_0 = arg_33_0:getLockedCard()

	if not var_33_0 then
		return
	end

	arg_33_1:setInfo(var_33_0.gameinfo, true)
	arg_33_1:disable()

	arg_33_0.unlock_event_card = arg_33_1

	if system.isLockStarfox2() and system.isStarfoxClear() then
		CloverUnlock.DecideUnlock(arg_33_0)
	end
end

function var_0_0.RunTask(arg_34_0)
	var_0_0.is_running = true

	CloverTask.Register("unlock", var_0_0[arg_34_0])
end

function var_0_0.KillTask()
	var_0_0.is_running = false

	CloverTask.Unregister("unlock")
end

CloverUnlock = {
	DecideUnlock = function(arg_36_0)
		if not var_0_0.is_running then
			var_0_0.event:reset()

			local var_36_0 = {
				element = arg_36_0
			}

			var_0_0.arg = var_36_0

			var_0_0.RunTask("DecideTask")
		end
	end,
	IsRunning = function()
		return var_0_0.is_running
	end
}

function var_0_0.DecideTask(arg_38_0)
	local var_38_0 = var_0_0.event
	local var_38_1 = var_0_0.arg.element
	local var_38_2 = false
	local var_38_3 = {
		function()
			CloverPadUI:EnableUserInput(false, "unlock")
			CloverDemo.Disable()
			system.unlockStarfox2()
			var_38_0:next_index()
		end,
		function()
			var_38_0:wait(1.5)
			var_38_0:next_index()
		end,
		function()
			if var_38_1:setCursorOnStarfox2() then
				var_38_0:wait(0.7)
				var_38_0:next_index()
			end
		end,
		function()
			Main:setVolumeMainBGM(0.3)

			local var_42_0, var_42_1 = var_38_1:getLockedCard():getWorldPosition()

			var_38_1.unlock_event_card:setWorldPosition(var_42_0, var_42_1)
			var_38_1.unlock_event_card:enable()
			var_38_1.unlock_event_card.lock:enable()
			var_38_1.unlock_event_card.unlock_dialog_message:disable()
			sys_boot:getInstance().fade:Out(0.1, nil, nil)
			var_38_0:next_index()
		end,
		function()
			var_38_0:wait(0.3)
			var_38_0:next_index()
		end,
		function()
			var_38_1.menu:disable()
			CloverBG.ResumeOn()
			var_38_0:next_index()
		end,
		function()
			Tween:sequence(Tween:moveTo(var_38_1.unlock_event_card, 1.5, 0, 0, Ease.inOutQuart)):start()
			var_38_0:wait(1.8)
			var_38_0:next_index()
		end,
		function()
			var_38_1.unlock_event_card:unlock_openbox_start()
			var_38_0:next_index()
		end,
		function()
			if var_38_1.unlock_event_card:unlock_wait_anime() then
				var_38_1.unlock_event_card:unlock_openbox_loop()
				var_38_0:next_index()
			end
		end,
		function()
			if var_38_1.unlock_event_card:unlock_wait_anime() then
				var_38_1.unlock_event_card:unlock_openbox_end()
				var_38_0:wait(1)
				var_38_0:next_index()
			end
		end,
		function()
			var_38_0:wait(0.5)
			var_38_1.unlock_event_card.unlock_dialog_message:disable()
			var_38_0:next_index()
		end,
		function()
			local var_50_0 = var_38_1:getLockedCard()

			var_50_0:disable()

			local var_50_1, var_50_2 = var_50_0:getWorldPosition()

			Tween:sequence(Tween:moveTo(var_38_1.unlock_event_card, 0.75, var_50_1, var_50_2, Ease.outExpo)):start()
			var_38_0:wait(0.2)
			var_38_0:next_index()
		end,
		function()
			if var_38_1.thumbnail_node then
				var_38_1.thumbnail_node:setUnselected(nil)
			end

			var_38_1:getLockedCard().lock:disable()
			var_38_1.menu:enable()
			CloverBG.ResumeOff()
			sys_boot:getInstance().fade:In(0.2, nil, nil)
			var_38_0:wait(0.2)
			var_38_0:next_index()
		end,
		function()
			Main:resetVolumeMainBGM()
			var_38_0:wait(0.4)
			var_38_0:next_index()
		end,
		function()
			var_38_1:getLockedCard():enable()
			var_38_1.unlock_event_card:disable()
			Main:toHomeMenu()
			CloverDemo.Enable()
			CloverPadUI:EnableUserInput(true, "unlock")

			var_38_2 = true
		end
	}

	var_38_0:update(var_38_3, arg_38_0)

	if var_38_2 then
		var_0_0.KillTask()
	end
end

function sys_gametitlelist.cardColorDark(arg_54_0, arg_54_1)
	local var_54_0 = #arg_54_0.elementArray
	local var_54_1 = arg_54_0.elementArray
	local var_54_2 = table_find(var_54_1, arg_54_0.current)
	local var_54_3, var_54_4 = arg_54_0.elementArray[var_54_2]:getWorldPosition()
	local var_54_5

	if var_54_3 <= -393 then
		var_54_5 = index_loop_prev(var_54_2, var_54_0, 2)
	elseif var_54_3 <= -131 then
		var_54_5 = index_loop_prev(var_54_2, var_54_0, 3)
	elseif var_54_3 <= 131 then
		var_54_5 = index_loop_prev(var_54_2, var_54_0, 4)
	else
		var_54_5 = index_loop_prev(var_54_2, var_54_0, 5)
	end

	if arg_54_1 then
		for iter_54_0 = 1, 6 do
			local var_54_6 = index_loop_next(var_54_5, var_54_0, iter_54_0)
			local var_54_7 = arg_54_0.elementArray[var_54_6]

			if var_54_2 ~= var_54_6 then
				if arg_54_0:isSelectable(var_54_7.gameinfo.game_code) then
					var_54_7:cardColorToDark(true, nil, true)
				else
					var_54_7:cardColorToDark(true, nil, nil)
				end
			end
		end
	else
		for iter_54_1 = 1, 6 do
			local var_54_8 = index_loop_next(var_54_5, var_54_0, iter_54_1)

			arg_54_0.elementArray[var_54_8]:cardColorToDark(nil, true, nil)
		end
	end
end

function sys_gametitlelist.showReplayText(arg_55_0)
	arg_55_0.sort_order:showReplayText()
end

function sys_gametitlelist.setNextCursorZ(arg_56_0, arg_56_1)
	arg_56_0.next_cursor_z = arg_56_1
end
