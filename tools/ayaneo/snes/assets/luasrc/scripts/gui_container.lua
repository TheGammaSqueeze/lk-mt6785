require("/scripts/core/core.lua")
require("/scripts/app/clover_pad.lua")

gui_container = class(GUIElement)

function gui_container.start(arg_1_0)
	if not arg_1_0.elements then
		arg_1_0.elements = arg_1_0:getChildByName("elements")

		if not arg_1_0.elements then
			arg_1_0.elements = createWorldNode("elements")

			arg_1_0.elements:setZIndex(1)
			arg_1_0:addChildNode(arg_1_0.elements)
		end
	end

	arg_1_0:buildElements()
end

function gui_container.buildElements(arg_2_0)
	local var_2_0 = arg_2_0.layoutType
	local var_2_1 = arg_2_0.elementType
	local var_2_2 = arg_2_0.elementArray

	if arg_2_0.elementArray == nil then
		var_2_2 = getObjects(arg_2_0.elements, var_2_1 or GUIElement)

		local var_2_3 = sort_positionComp

		if arg_2_0.neighborDirection == "vertical_glid" then
			var_2_3 = sort_positionCompV
		end

		table.sort(var_2_2, var_2_3)
	end

	if table_isEmpty(var_2_2) then
		print("nodes empty!! " .. arg_2_0:getName())
	end

	if arg_2_0.layoutType then
		arg_2_0.layout = new(var_2_0)

		arg_2_0.layout:suspend()

		local var_2_4, var_2_5 = arg_2_0.elements:getWorldPosition()
		local var_2_6 = arg_2_0.HGap
		local var_2_7 = arg_2_0.VGap
		local var_2_8 = var_2_2[1]
		local var_2_9 = var_2_2[2]

		if var_2_8 and arg_2_0.autoPosition then
			var_2_4, var_2_5 = var_2_8:getWorldPosition()
		end

		if var_2_8 and var_2_9 and arg_2_0.autoGap then
			local var_2_10, var_2_11 = var_2_8:getWorldPosition()
			local var_2_12, var_2_13 = var_2_9:getWorldPosition()

			var_2_6, var_2_7 = var_2_12 - var_2_10, var_2_13 - var_2_11
			var_2_7 = -var_2_7
		end

		arg_2_0.layout:initialize(var_2_6, var_2_7)
		arg_2_0.elements:setWorldPosition(var_2_4, var_2_5)

		for iter_2_0, iter_2_1 in ipairs(var_2_2) do
			local var_2_14 = iter_2_1

			arg_2_0.layout:pushBackElement(var_2_14)
		end

		arg_2_0.layout:resume(true)
	end

	for iter_2_2, iter_2_3 in ipairs(var_2_2) do
		local var_2_15 = iter_2_3

		if arg_2_0.setListener then
			var_2_15.listener = arg_2_0
		end

		var_2_15.preListener = arg_2_0
	end

	arg_2_0.elementArray = var_2_2

	if arg_2_0.focus and table_find(var_2_2, arg_2_0.focus) then
		arg_2_0.current = arg_2_0.focus
	else
		arg_2_0.current = var_2_2[1]
	end

	arg_2_0.repeatedTimes = 0
end

function gui_container.activate(arg_3_0)
	arg_3_0.isActive = true

	Host.setEnabled(arg_3_0, true)
	gui_custom:popGUIStatus()
	gui_custom:pushGUIStatus(arg_3_0.guiStatusName)

	if arg_3_0.activeAnimation then
		tween_stop(arg_3_0.activeTweenInst)

		arg_3_0.activeTweenInst = arg_3_0.activeAnimation:run(arg_3_0)
	end

	GUI:focusElement(arg_3_0.current, true)
end

function gui_container.deactivate(arg_4_0)
	arg_4_0.isActive = false

	if arg_4_0.deactiveAnimation then
		-- the deactive animation (slide_out) moves the node off-screen and disables it at the
		-- end; hiding it now would make it animate invisibly (the submenu would vanish instead
		-- of sliding out). Let the animation handle the visual hide.
		tween_stop(arg_4_0.activeTweenInst)

		arg_4_0.activeTweenInst = arg_4_0.deactiveAnimation:run(arg_4_0)
	else
		Host.setEnabled(arg_4_0, false)
	end
end

function gui_container.isFocused(arg_5_0)
	return arg_5_0 == GUI.focusedElement
end

function gui_container.elementIsFocused(arg_6_0)
	return table_find(arg_6_0.elementArray, GUI.focusedElement) ~= nil
end

function gui_container.elementIsFocusedRecursive(arg_7_0)
	for iter_7_0, iter_7_1 in pairs(arg_7_0.elementArray) do
		if iter_7_1.elementIsFocusedRecursive and iter_7_1:elementIsFocusedRecursive() then
			return true
		end

		if iter_7_1 == GUI.focusedElement then
			return true
		end
	end

	return false
end

function gui_container.onElementFocus(arg_8_0, arg_8_1)
	if not arg_8_0.isActive then
		arg_8_0:activate()
	end

	if arg_8_1 and arg_8_1 ~= arg_8_0 then
		if arg_8_1.activate then
			arg_8_1:activate()
		end

		arg_8_0.current = arg_8_1
	end
end

function gui_container.onElementUnfocus(arg_9_0, arg_9_1)
	if arg_9_1 and arg_9_1 ~= arg_9_0 and arg_9_1.deactivate then
		arg_9_1:deactivate()
	end
end

function gui_container.onElementCommand(arg_10_0, arg_10_1, arg_10_2)
	if arg_10_0:forwardCommandToPreListener(arg_10_2) then
		return true
	end

	local var_10_0 = arg_10_0.elementArray
	local var_10_1 = arg_10_0.fixed_node_num or #var_10_0
	local var_10_2 = table_find(var_10_0, arg_10_1)

	local function var_10_3(arg_11_0, arg_11_1)
		if not GUI.repeatedThisFrame then
			arg_10_0.repeatedTimes = 0

			return true
		end

		arg_10_0.repeatedTimes = arg_10_0.repeatedTimes + 1

		if not arg_11_0 then
			return true
		end

		if arg_10_0.repeatedTimes > 1 then
			GUI.state = 1
			GUI.startPressedTime = GUI.currentTime - arg_11_1.REPEAT_DELAY + arg_11_1.REPEAT_LOOPED_STOP_DELAY
			arg_10_0.repeatedTimes = 0

			return false
		end

		GUI.state = 1
		GUI.startPressedTime = GUI.currentTime - arg_11_1.REPEAT_DELAY + arg_11_1.REPEAT_LOOPED_RESTART_DELAY

		return true
	end

	if var_10_2 and arg_10_2.id == GUI_COMMAND_KEYPRESS then
		if type(var_10_2) == "number" then
			local var_10_4 = var_10_2

			repeat
				var_10_4 = index_loop_prev(var_10_4, var_10_1)
			until elementIsEnabled(var_10_0[var_10_4]) or var_10_4 == var_10_2

			local var_10_5

			var_10_5 = var_10_2 <= var_10_4

			local var_10_6 = var_10_2

			repeat
				var_10_6 = index_loop_next(var_10_6, var_10_1)
			until elementIsEnabled(var_10_0[var_10_6]) or var_10_6 == var_10_2

			local var_10_7

			var_10_7 = var_10_6 <= var_10_2

			if arg_10_0.neighborDirection == "holizontal" then
				if CloverPadUI.pressed.left then
					if var_10_3(var_10_4 == var_10_1, GUI.H) then
						GUI:focusElement(var_10_0[var_10_4])
					end

					return true
				elseif CloverPadUI.pressed.right then
					if var_10_3(var_10_6 == 1, GUI.H) then
						GUI:focusElement(var_10_0[var_10_6])
					end

					return true
				elseif not CloverPadUI.down.left and not CloverPadUI.down.right then
					if CloverPadUI.pressed.up and elementIsEnabled(arg_10_0.upElement) then
						GUI:focusElement(arg_10_0.upElement)
						arg_10_0:deactivate()

						return true
					elseif CloverPadUI.pressed.down and elementIsEnabled(arg_10_0.downElement) then
						GUI:focusElement(arg_10_0.downElement)
						arg_10_0:deactivate()

						return true
					end
				end
			end

			if arg_10_0.neighborDirection == "vertical" then
				if CloverPadUI.pressed.up then
					if var_10_3(var_10_4 == var_10_1, GUI.V) then
						GUI:focusElement(var_10_0[var_10_4])
					end

					return true
				elseif CloverPadUI.pressed.down then
					if var_10_3(var_10_6 == 1, GUI.V) then
						GUI:focusElement(var_10_0[var_10_6])
					end

					return true
				elseif not CloverPadUI.down.up and not CloverPadUI.down.down then
					if CloverPadUI.pressed.left and elementIsEnabled(arg_10_0.leftElement) then
						GUI:focusElement(arg_10_0.leftElement)
						arg_10_0:deactivate()

						return true
					elseif CloverPadUI.pressed.right and elementIsEnabled(arg_10_0.rightElement) then
						GUI:focusElement(arg_10_0.rightElement)
						arg_10_0:deactivate()

						return true
					end
				end
			end

			if arg_10_0.neighborDirection == "vertical_glid" then
				local var_10_8 = arg_10_0.row
				local var_10_9 = var_10_2
				local var_10_10 = false

				repeat
					local var_10_11, var_10_12 = index_vertical_grid(var_10_9, var_10_1, var_10_8, "column", -1)

					var_10_9 = var_10_11
					var_10_10 = var_10_10 or var_10_12
				until elementIsEnabled(var_10_0[var_10_9]) or var_10_9 == var_10_2

				local var_10_13 = var_10_2
				local var_10_14 = false

				repeat
					local var_10_15, var_10_16 = index_vertical_grid(var_10_13, var_10_1, var_10_8, "column", 1)

					var_10_13 = var_10_15
					var_10_14 = var_10_14 or var_10_16
				until elementIsEnabled(var_10_0[var_10_13]) or var_10_13 == var_10_2

				local var_10_17 = var_10_2
				local var_10_18 = false

				repeat
					local var_10_19, var_10_20 = index_vertical_grid(var_10_17, var_10_1, var_10_8, "row", -1)

					var_10_17 = var_10_19
					var_10_18 = var_10_18 or var_10_20
				until elementIsEnabled(var_10_0[var_10_17]) or var_10_17 == var_10_2

				local var_10_21 = var_10_2
				local var_10_22 = false

				repeat
					local var_10_23, var_10_24 = index_vertical_grid(var_10_21, var_10_1, var_10_8, "row", 1)

					var_10_21 = var_10_23
					var_10_22 = var_10_22 or var_10_24
				until elementIsEnabled(var_10_0[var_10_21]) or var_10_21 == var_10_2

				if CloverPadUI.pressed.up then
					if var_10_10 and elementIsEnabled(arg_10_0.upElement) then
						GUI:focusElement(arg_10_0.upElement)
					else
						GUI:focusElement(var_10_0[var_10_9])
					end

					return true
				elseif CloverPadUI.pressed.down then
					if var_10_14 and elementIsEnabled(arg_10_0.downElement) then
						GUI:focusElement(arg_10_0.downElement)
					else
						GUI:focusElement(var_10_0[var_10_13])
					end

					return true
				elseif not arg_10_0.disabledVertical then
					if CloverPadUI.pressed.left then
						if var_10_18 and elementIsEnabled(arg_10_0.leftElement) then
							GUI:focusElement(arg_10_0.leftElement)
						else
							GUI:focusElement(var_10_0[var_10_17])
						end

						return true
					elseif CloverPadUI.pressed.right then
						if var_10_22 and elementIsEnabled(arg_10_0.rightElement) then
							GUI:focusElement(arg_10_0.rightElement)
						else
							GUI:focusElement(var_10_0[var_10_21])
						end

						return true
					end
				end
			end
		elseif CloverPadUI.pressed.left and elementIsEnabled(arg_10_0.leftElement) then
			GUI:focusElement(arg_10_0.leftElement)
			arg_10_0:deactivate()

			return true
		elseif CloverPadUI.pressed.right and elementIsEnabled(arg_10_0.rightElement) then
			GUI:focusElement(arg_10_0.rightElement)
			arg_10_0:deactivate()

			return true
		elseif CloverPadUI.pressed.up and elementIsEnabled(arg_10_0.upElement) then
			GUI:focusElement(arg_10_0.upElement)
			arg_10_0:deactivate()

			return true
		elseif CloverPadUI.pressed.down and elementIsEnabled(arg_10_0.downElement) then
			GUI:focusElement(arg_10_0.downElement)
			arg_10_0:deactivate()

			return true
		end

		if CloverPadUI:isCancelPressed() and arg_10_0.onContainerCanceled then
			local var_10_25 = arg_10_0:onContainerCanceled(arg_10_1)

			if var_10_25 == nil then
				return true
			else
				return var_10_25
			end
		end
	end

	return false
end
