require("/scripts/core/core.lua")
require("/scripts/helper_nodes.lua")
require("/scripts/system.lua")
require("/scripts/DecorativeFrames.lua")
require("/scripts/app/clover_autocancel.lua")

sys_option_display = class(gui_container)

function sys_option_display.start(arg_1_0)
	gui_container.start(arg_1_0)

	if store.setting == nil then
		store.setting = {}
	end

	local var_1_0 = table_find_if(arg_1_0.elementArray, function(arg_2_0)
		return arg_2_0.parameter == store.setting.display
	end)

	assert(var_1_0)
	DecorativeFrames:init()
end

function sys_option_display.update(arg_3_0)
	if not arg_3_0.is_setup then
		if DecorativeFrames:areLoading() then
			return
		end

		DecorativeFrames:checkLoadErrors()
		arg_3_0:addLoadedFrames()
		arg_3_0:setup()

		if arg_3_0.is_activate_requested then
			arg_3_0:activate()

			arg_3_0.is_activate_requested = nil
		end
	end

	gui_container.update(arg_3_0)
end

function sys_option_display.addLoadedFrames(arg_4_0)
	local var_4_0 = 0
	local var_4_1 = 0

	for iter_4_0 in iterate_children(arg_4_0.elements_frame_root) do
		local var_4_2, var_4_3 = iter_4_0:getLocalPosition()

		var_4_0 = math.max(var_4_0, var_4_2)
		var_4_1 = math.min(var_4_1, var_4_3)
	end

	for iter_4_1 = 1, DecorativeFrames:getFrameCount() do
		local var_4_4 = arg_4_0.frame_item_prefab:instantiate()

		arg_4_0.elements_frame_root:addChildNode(var_4_4)

		var_4_0 = var_4_0 + 1

		var_4_4:setLocalPosition(var_4_0, var_4_1)
		var_4_4.frameTextureComponent:setTexture(DecorativeFrames:getThumbnailTexture(iter_4_1))

		for iter_4_2, iter_4_3 in ipairs(arg_4_0.elementArray) do
			local var_4_5 = DecorativeFrames:getPreviewTexture(iter_4_1, iter_4_3.parameter)

			table.insert(iter_4_3.displays, var_4_5)
		end
	end
end

function sys_option_display.setup(arg_5_0)
	for iter_5_0, iter_5_1 in ipairs(arg_5_0.elementArray) do
		arg_5_0.radiogroup:removeRadioButton(iter_5_1)
	end

	for iter_5_2, iter_5_3 in ipairs(arg_5_0.elementArray) do
		arg_5_0.radiogroup:addRadioButton(iter_5_3)
	end

	arg_5_0.displayNodeArray = {}

	for iter_5_4, iter_5_5 in ipairs(arg_5_0.elementArray) do
		table.insert(arg_5_0.displayNodeArray, iter_5_5)
	end

	arg_5_0.frameNodeArray = {}

	for iter_5_6 in iterate_children(arg_5_0.elements_frame_root) do
		arg_5_0.radiogroupFrame:removeRadioButton(iter_5_6)
		table.insert(arg_5_0.frameNodeArray, iter_5_6)
	end

	table.sort(arg_5_0.frameNodeArray, sort_positionComp)

	for iter_5_7, iter_5_8 in ipairs(arg_5_0.frameNodeArray) do
		arg_5_0.radiogroupFrame:addRadioButton(iter_5_8)
	end

	local var_5_0 = -((arg_5_0.frame_display_num - 1) / 2) * arg_5_0.frame_interval

	for iter_5_9, iter_5_10 in ipairs(arg_5_0.frameNodeArray) do
		local var_5_1 = var_5_0 + (iter_5_9 - 1) * arg_5_0.frame_interval

		iter_5_10:setLocalPosition(var_5_1, 0)
	end

	arg_5_0:resetFrameScroll()

	arg_5_0.elements_lines = {
		{
			id = "display",
			isBorderLoop = true,
			root_node = arg_5_0:getChildByName("elements")
		},
		{
			id = "frame",
			isBorderLoop = false,
			root_node = arg_5_0.elements_frame_root,
			focused_visible_node = arg_5_0.focused_nodes_frame
		}
	}
	arg_5_0.current_line_index = 1
	arg_5_0.current_line_id = "display"

	arg_5_0.focused_nodes_frame:disable()
	arg_5_0:setCursor()
	arg_5_0:updateDisplayFrame()

	arg_5_0.is_setup = true
end

function sys_option_display.activate(arg_6_0)
	if not arg_6_0.is_setup then
		arg_6_0.is_activate_requested = true

		return
	end

	arg_6_0:resetFrameScroll()

	for iter_6_0, iter_6_1 in ipairs(arg_6_0.elements_lines) do
		iter_6_1.focused_node = nil
	end

	arg_6_0:updateLineSelect(1)
	arg_6_0:setCursor()
	arg_6_0:updateDisplayFrame()
	gui_container.activate(arg_6_0)
	CloverAutoCancel.Set("sys_option_display", arg_6_0)
end

function sys_option_display.setCursor(arg_7_0)
	if not store.setting then
		store.setting = {}
	end

	if arg_7_0.current_line_id == "display" then
		local var_7_0 = table_find_if(arg_7_0.displayNodeArray, function(arg_8_0)
			return arg_8_0.parameter == store.setting.display
		end) or 1

		arg_7_0.current = arg_7_0.displayNodeArray[var_7_0]

		arg_7_0.radiogroup:selectIndex(var_7_0)
	elseif arg_7_0.current_line_id == "frame" then
		arg_7_0.current = arg_7_0.frameNodeArray[arg_7_0.selected_frame_index]

		arg_7_0.radiogroupFrame:selectIndex(store.setting.frame_index)
	end
end

function sys_option_display.resetFrameScroll(arg_9_0)
	if not store.setting then
		store.setting = {}
	end

	arg_9_0.selected_frame_index = store.setting.frame_index
	arg_9_0.scroll_index = store.setting.frame_scroll

	if not system.isPlayingDemo() then
		if store.setting.frame_index then
			store.setting.frame_index = math.min(store.setting.frame_index, #arg_9_0.frameNodeArray)
			arg_9_0.selected_frame_index = store.setting.frame_index
			store.setting.frame_index = arg_9_0.selected_frame_index
		end
	else
		arg_9_0.selected_frame_index = 1
		arg_9_0.scroll_index = 0
	end

	arg_9_0.scroll_value = 0

	local var_9_0 = false
	local var_9_1 = false
	local var_9_2 = arg_9_0.selected_frame_index - 1

	if var_9_2 < arg_9_0.scroll_index then
		var_9_0 = true
	elseif var_9_2 >= arg_9_0.scroll_index + arg_9_0.frame_display_num then
		var_9_1 = true
	end

	if var_9_1 then
		arg_9_0.scroll_index = arg_9_0.selected_frame_index - arg_9_0.frame_display_num
	elseif var_9_0 then
		arg_9_0.scroll_index = var_9_2
	end

	arg_9_0:resetFrameScrollDisplay()
	arg_9_0.radiogroupFrame:selectIndex(arg_9_0.selected_frame_index)
	arg_9_0:updateFrameArrow()
end

function sys_option_display.resetFrameScrollDisplay(arg_10_0)
	for iter_10_0, iter_10_1 in ipairs(arg_10_0.frameNodeArray) do
		iter_10_1:disable()
	end

	local var_10_0 = arg_10_0.scroll_index + 1
	local var_10_1 = arg_10_0.scroll_index + arg_10_0.frame_display_num
	local var_10_2 = math.min(var_10_1, #arg_10_0.frameNodeArray)

	for iter_10_2 = var_10_0, var_10_2 do
		arg_10_0.frameNodeArray[iter_10_2]:enable()
	end

	arg_10_0.scroll_value = -arg_10_0.scroll_index * arg_10_0.frame_interval

	arg_10_0.elements_frame_root:setLocalPositionX(arg_10_0.scroll_value)
end

function sys_option_display.updateLineSelect(arg_11_0, arg_11_1)
	if arg_11_1 <= 0 then
		arg_11_1 = #arg_11_0.elements_lines
	end

	if arg_11_1 > #arg_11_0.elements_lines then
		arg_11_1 = 1
	end

	if arg_11_0.current_line_index == arg_11_1 then
		return
	end

	local var_11_0 = arg_11_0.elements_lines[arg_11_0.current_line_index]
	local var_11_1 = arg_11_0.elements_lines[arg_11_1]

	arg_11_0.current_line_index = arg_11_1
	arg_11_0.current_line_id = var_11_1.id

	if var_11_0.focused_visible_node then
		var_11_0.focused_visible_node:disable()
	end

	if var_11_1.focused_visible_node then
		var_11_1.focused_visible_node:enable()
	end

	var_11_0.focused_node = GUI.focusedElement
	arg_11_0.elements = var_11_1.root_node
	arg_11_0.elementArray = nil

	arg_11_0:buildElements()

	if var_11_1.focused_node then
		arg_11_0.current = var_11_1.focused_node
	else
		arg_11_0:setCursor()
	end

	GUI:focusElement(arg_11_0.current)

	if var_11_1.id == "frame" then
		local var_11_2 = arg_11_0.current:getChildByName("cursor_area"):getComponent(SpriteComponent)

		system.cursor:setSquare(var_11_2, true)

		GUI.REPEAT_DELAY = 0.24
		GUI.REPEAT_RATE = 0.09
	else
		system.cursor:cursorHide()

		GUI.REPEAT_DELAY = GUI.H.REPEAT_DELAY
		GUI.REPEAT_RATE = GUI.H.REPEAT_RATE
	end
end

function sys_option_display.updateDisplayFrame(arg_12_0)
	local var_12_0 = store.setting.frame_index

	if system.isPlayingDemo() then
		var_12_0 = 1
	end

	for iter_12_0, iter_12_1 in ipairs(arg_12_0.displayNodeArray) do
		local var_12_1 = iter_12_1.displays[var_12_0]

		if var_12_1 then
			iter_12_1:getChildByName("display"):getComponent(TextureComponent):setTexture(var_12_1)
		end
	end
end

function sys_option_display.updateFrameArrow(arg_13_0)
	if arg_13_0.scroll_index == 0 then
		arg_13_0.arrow_left_on:disable()
		arg_13_0.arrow_left_off:enable()
	else
		arg_13_0.arrow_left_on:enable()
		arg_13_0.arrow_left_off:disable()
	end

	local var_13_0 = #arg_13_0.frameNodeArray - arg_13_0.frame_display_num

	if arg_13_0.scroll_index == var_13_0 then
		arg_13_0.arrow_right_on:disable()
		arg_13_0.arrow_right_off:enable()
	else
		arg_13_0.arrow_right_on:enable()
		arg_13_0.arrow_right_off:disable()
	end
end

function sys_option_display.onFrameChange(arg_14_0)
	arg_14_0:resetFrameScrollDisplay()

	local var_14_0 = arg_14_0.frameNodeArray[arg_14_0.selected_frame_index]:getChildByName("cursor_area"):getComponent(SpriteComponent)

	system.cursor:setSquare(var_14_0, true)
end

function sys_option_display.onRadioSelectionChanged(arg_15_0, arg_15_1)
	if not store.setting then
		store.setting = {}
	end

	if arg_15_0.current_line_id == "display" then
		local var_15_0 = arg_15_1.selected

		store.setting.display = arg_15_0.elementArray[var_15_0].parameter
	elseif arg_15_0.current_line_id == "frame" then
		store.setting.frame_index = arg_15_1.selected

		arg_15_0:updateDisplayFrame()
	end
end

function sys_option_display.onContainerCanceled(arg_16_0, arg_16_1)
	CloverAutoCancel.Set("sys_option_display", nil)
	Main:toHomeMenu()

	if arg_16_0.cancelSound then
		arg_16_0.cancelSound:stop()
		arg_16_0.cancelSound:play()
	end
end

function sys_option_display.onElementCommand(arg_17_0, arg_17_1, arg_17_2)
	if (function()
		if arg_17_2.id == GUI_COMMAND_KEYPRESS and not CloverPadUI.down.left and not CloverPadUI.down.right and (CloverPadUI.pressed.up or CloverPadUI.pressed.down) then
			return true
		end

		return false
	end)() then
		if CloverPadUI.pressed.up then
			arg_17_0:updateLineSelect(arg_17_0.current_line_index - 1)
		elseif CloverPadUI.pressed.down then
			arg_17_0:updateLineSelect(arg_17_0.current_line_index + 1)
		end

		return
	end

	if arg_17_0.current_line_id == "frame" and arg_17_2.id == GUI_COMMAND_KEYPRESS then
		if CloverPadUI.pressed.left then
			if arg_17_0.selected_frame_index == 1 then
				return
			end

			if arg_17_0.selected_frame_index - 1 == arg_17_0.scroll_index then
				arg_17_0.scroll_index = arg_17_0.scroll_index - 1
			end

			arg_17_0.selected_frame_index = arg_17_0.selected_frame_index - 1

			arg_17_0:onFrameChange()
		elseif CloverPadUI.pressed.right then
			if arg_17_0.selected_frame_index == #arg_17_0.frameNodeArray then
				return
			end

			if arg_17_0.selected_frame_index == arg_17_0.scroll_index + arg_17_0.frame_display_num then
				arg_17_0.scroll_index = arg_17_0.scroll_index + 1
			end

			arg_17_0.selected_frame_index = arg_17_0.selected_frame_index + 1

			arg_17_0:onFrameChange()
		end

		store.setting.frame_scroll = arg_17_0.scroll_index

		arg_17_0:updateFrameArrow()
	end

	gui_container.onElementCommand(arg_17_0, arg_17_1, arg_17_2)
end
