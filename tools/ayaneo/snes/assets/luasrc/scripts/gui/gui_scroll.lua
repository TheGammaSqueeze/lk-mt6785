require("gui.lua")

local var_0_0 = math.abs
local var_0_1 = Math.round
local var_0_2 = Math.clamp
local var_0_3 = Math.lerp
local var_0_4 = math.min
local var_0_5 = math.max
local var_0_6 = math.huge

GUIScroll = class(GUIElement)

function GUIScroll.start(arg_1_0)
	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_1_0) .. "]\tGUIScroll:start()")
	end

	GUIElement.start(arg_1_0)

	arg_1_0.scrollXValue = 0
	arg_1_0.scrollYValue = 0
	arg_1_0.speedX = 0
	arg_1_0.speedY = 0
	arg_1_0.startX = 0
	arg_1_0.startY = 0
	arg_1_0.width = 0
	arg_1_0.height = 0
	arg_1_0.elementsCount = 0
	arg_1_0.isDragging = false
	arg_1_0.isDragEngaged = false
	arg_1_0.computeInertia = false

	if not arg_1_0.dragTreshold then
		arg_1_0.dragTreshold = 5
	end

	if not arg_1_0.direction then
		arg_1_0.direction = "horizontal"
	end

	if not arg_1_0.layout then
		if arg_1_0.direction == "horizontal" then
			arg_1_0.layout = "horizontal_list"
		elseif arg_1_0.direction == "vertical" then
			arg_1_0.layout = "vertical_list"
		end
	end

	if arg_1_0.layout == "horizontal_list" then
		arg_1_0.rowsCount = 1
		arg_1_0.columnsCount = var_0_6
		arg_1_0.layoutObj = new(GUIHLayout)
	elseif arg_1_0.layout == "vertical_list" then
		arg_1_0.rowsCount = var_0_6
		arg_1_0.columnsCount = 1
		arg_1_0.layoutObj = new(GUIVLayout)
	elseif arg_1_0.layout == "grid" then
		if arg_1_0.rowsCount == nil then
			arg_1_0.rowsCount = 2
		end

		if arg_1_0.columnsCount == nil then
			arg_1_0.columnsCount = 2
		end
	end

	arg_1_0.layoutObj:initialize(arg_1_0.HGap, arg_1_0.VGap)

	if not arg_1_0.hasInnerScroll then
		arg_1_0.hasInnerScroll = false
	end

	if not arg_1_0.elements then
		arg_1_0.elements = arg_1_0:getChildByName("elements")

		if not arg_1_0.elements then
			arg_1_0.elements = createWorldNode("elements")

			arg_1_0.elements:setZIndex(1)
			arg_1_0:addChildNode(arg_1_0.elements)
		end
	end

	arg_1_0:setupClickableArea()

	local var_1_0, var_1_1 = arg_1_0.clickable:getBoxShapeSize()

	if not arg_1_0.maxWidth then
		arg_1_0.maxWidth = var_1_0
	end

	if not arg_1_0.maxHeight then
		arg_1_0.maxHeight = var_1_1
	end

	if arg_1_0.dragFactorX == nil then
		arg_1_0.dragFactorX = 1
	end

	if arg_1_0.dragFactorY == nil then
		arg_1_0.dragFactorY = 1
	end

	if arg_1_0.friction == nil then
		arg_1_0.friction = 5
	end
end

function GUIScroll.update(arg_2_0, arg_2_1)
	arg_2_0.dt = arg_2_1

	if arg_2_0.hasInertia and arg_2_0.computeInertia then
		local var_2_0 = arg_2_0.speedX
		local var_2_1 = arg_2_0.speedY
		local var_2_2 = arg_2_0.friction

		arg_2_0:scrollBy(var_2_0 * arg_2_1, var_2_1 * arg_2_1)

		local var_2_3 = var_0_3(var_2_0, 0, arg_2_1 * var_2_2)
		local var_2_4 = var_0_3(var_2_1, 0, arg_2_1 * var_2_2)

		if var_0_0(var_2_3) < 1 and var_0_0(var_2_4) < 1 then
			arg_2_0.computeInertia = false
		end

		arg_2_0.speedX = var_2_3
		arg_2_0.speedY = var_2_4
	end
end

function GUIScroll.stopScroll(arg_3_0)
	arg_3_0.computeInertia = false
	arg_3_0.speedX = 0
	arg_3_0.speedY = 0
end

function GUIScroll.onElementCommand(arg_4_0, arg_4_1, arg_4_2)
	if arg_4_0:forwardCommandToPreListener(arg_4_2) then
		return true
	end

	if arg_4_2.id == GUI_COMMAND_PRESS then
		arg_4_0:stopScroll()

		return false
	end

	return false
end

function GUIScroll.onCommand(arg_5_0, arg_5_1)
	if GUI.DEBUG_LEVEL.commands then
		GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUIScroll:onCommand() " .. tostring(arg_5_1.id))
	end

	if arg_5_1.id == GUI_COMMAND_PRESS then
		arg_5_0:stopScroll()

		return true
	elseif arg_5_1.id == GUI_COMMAND_DRAG then
		local var_5_0, var_5_1 = arg_5_0:worldToLocalPosition(arg_5_1.posX, arg_5_1.posY)
		local var_5_2, var_5_3 = arg_5_0:worldToLocalPosition(arg_5_1.previousPosX, arg_5_1.previousPosY)

		if arg_5_1.first then
			arg_5_0.isDragEngaged = false
		end

		if not arg_5_0.isDragging then
			if not arg_5_0.isDragEngaged then
				arg_5_0.isDragEngaged = true

				if GUI.DEBUG_LEVEL.dragging then
					GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUIScroll drag engaged")
				end

				local var_5_4 = var_0_0(var_5_0 - var_5_2) > var_0_0(var_5_1 - var_5_3)

				if arg_5_0.direction == "horizontal" and not var_5_4 or arg_5_0.direction == "vertical" and var_5_4 then
					if GUI.DEBUG_LEVEL.dragging then
						GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUIScroll wrong direction ")
					end

					return false
				end

				local var_5_5, var_5_6 = arg_5_0.clickable:getBoxShapeSize()

				if var_5_4 and not arg_5_0.hasInnerScroll and var_5_5 > arg_5_0.layoutObj:getWidth() then
					return false
				end

				if not var_5_4 and not arg_5_0.hasInnerScroll and var_5_6 > arg_5_0.layoutObj:getHeight() then
					return false
				end

				arg_5_0.startX = var_5_2
				arg_5_0.startY = var_5_3

				arg_5_0:stopScroll()

				arg_5_0.isDragging = true
			else
				if GUI.DEBUG_LEVEL.dragging then
					GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUIScroll drag already engaged")
				end

				return false
			end
		else
			local var_5_7 = var_5_0 - var_5_2
			local var_5_8 = var_5_1 - var_5_3

			arg_5_0.speedX = var_5_7 / arg_5_0.dt
			arg_5_0.speedY = var_5_8 / arg_5_0.dt
			arg_5_0.startX = var_5_0
			arg_5_0.startY = var_5_1

			arg_5_0:scrollBy(var_5_7 * arg_5_0.dragFactorX, var_5_8 * arg_5_0.dragFactorY)

			if GUI.DEBUG_LEVEL.dragging then
				GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUIScroll dragging ")
			end
		end

		return true
	elseif arg_5_1.id == GUI_COMMAND_RELEASE then
		if GUI.DEBUG_LEVEL.actions then
			GUI:debugPrint("[" .. tostring(arg_5_0) .. "]\tGUIScroll released ")
		end

		arg_5_0.computeInertia = true
		arg_5_0.isDragging = false
		arg_5_0.isDragEngaged = false

		return true
	end

	return GUIElement.onCommand(arg_5_0, arg_5_1)
end

function GUIScroll.getBoundaries(arg_6_0)
	local var_6_0, var_6_1 = arg_6_0.clickable:getCenter()
	local var_6_2, var_6_3 = arg_6_0.clickable:getBoxShapeSize()
	local var_6_4 = -var_6_2 / 2 + var_6_0 - arg_6_0.layoutObj.xMin
	local var_6_5 = var_6_2 / 2 + var_6_0 - arg_6_0.layoutObj.xMax
	local var_6_6 = -var_6_3 / 2 + var_6_1 - arg_6_0.layoutObj.yMin
	local var_6_7 = var_6_3 / 2 + var_6_1 - arg_6_0.layoutObj.yMax
	local var_6_8 = var_0_4(var_6_4, var_6_5)
	local var_6_9 = var_0_5(var_6_4, var_6_5)
	local var_6_10 = var_0_4(var_6_6, var_6_7)
	local var_6_11 = var_0_5(var_6_6, var_6_7)

	return var_6_8, var_6_9, var_6_10, var_6_11
end

function GUIScroll.getScrollX(arg_7_0)
	return arg_7_0.scrollXValue
end

function GUIScroll.getScrollY(arg_8_0)
	return arg_8_0.scrollYValue
end

function GUIScroll.scrollBy(arg_9_0, arg_9_1, arg_9_2)
	if arg_9_0.direction == "vertical" then
		arg_9_1 = 0
	end

	if arg_9_0.direction == "horizontal" then
		arg_9_2 = 0
	end

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_9_0) .. "]\tGUIScroll:scrollBy() " .. tostring(arg_9_1) .. " " .. tostring(arg_9_2))
	end

	local var_9_0, var_9_1 = arg_9_0.elements:getLocalPosition()

	arg_9_0:scrollTo(var_9_0 + arg_9_1, var_9_1 + arg_9_2)
end

function GUIScroll.scrollTo(arg_10_0, arg_10_1, arg_10_2)
	local var_10_0, var_10_1, var_10_2, var_10_3 = arg_10_0:getBoundaries()
	local var_10_4 = var_0_2(arg_10_1, var_10_0, var_10_1)
	local var_10_5 = var_0_2(arg_10_2, var_10_2, var_10_3)

	if var_10_1 ~= var_10_0 then
		arg_10_0.scrollXValue = (var_10_4 - var_10_0) / (var_10_1 - var_10_0)
	else
		arg_10_0.scrollXValue = 0
	end

	if var_10_3 ~= var_10_2 then
		arg_10_0.scrollYValue = (var_10_5 - var_10_2) / (var_10_3 - var_10_2)
	else
		arg_10_0.scrollYValue = 0
	end

	local var_10_6, var_10_7 = arg_10_0.clickable:getBoxShapeSize()

	if var_10_6 <= arg_10_0.layoutObj:getWidth() then
		arg_10_0.scrollXValue = 1 - arg_10_0.scrollXValue
	end

	if var_10_7 <= arg_10_0.layoutObj:getHeight() then
		arg_10_0.scrollYValue = 1 - arg_10_0.scrollYValue
	end

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_10_0) .. "]\tGUIScroll:scrollTo() : scroll values = " .. tostring(arg_10_0.scrollXValue) .. "," .. tostring(arg_10_0.scrollYValue))
	end

	if GUI.roundedUnits then
		arg_10_0.elements:setLocalPosition(var_0_1(var_10_4), var_0_1(var_10_5))
	else
		arg_10_0.elements:setLocalPosition(var_10_4, var_10_5)
	end

	if arg_10_0.listener ~= nil and arg_10_0.listener.onScroll ~= nil then
		arg_10_0.listener:onScroll(arg_10_0, arg_10_0.scrollXValue, arg_10_0.scrollYValue)
	end
end

function GUIScroll.scrollPercentXY(arg_11_0, arg_11_1, arg_11_2)
	local var_11_0, var_11_1 = arg_11_0.clickable:getBoxShapeSize()

	arg_11_0.scrollXValue = var_0_2(arg_11_1, 0, 1)
	arg_11_0.scrollYValue = var_0_2(arg_11_2, 0, 1)

	if var_11_0 <= arg_11_0.layoutObj:getWidth() then
		arg_11_1 = 1 - arg_11_1
	end

	if var_11_1 >= arg_11_0.layoutObj:getHeight() then
		arg_11_2 = 1 - arg_11_2
	end

	local var_11_2, var_11_3, var_11_4, var_11_5 = arg_11_0:getBoundaries()

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_11_0) .. "]\tGUIScroll:scrollXY() : clamp X = [" .. var_11_2 .. ":" .. var_11_3 .. "]")
	end

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_11_0) .. "]\tGUIScroll:scrollXY() : clamp Y = [" .. var_11_4 .. ":" .. var_11_5 .. "]")
	end

	local var_11_6 = arg_11_1 * (var_11_3 - var_11_2) + var_11_2
	local var_11_7 = arg_11_2 * (var_11_5 - var_11_4) + var_11_4

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_11_0) .. "]\tGUIScroll:scrollXY() : pos  = " .. tostring(var_11_6) .. "," .. tostring(var_11_7))
	end

	if GUI.roundedUnits then
		arg_11_0.elements:setLocalPosition(var_0_1(var_11_6), var_0_1(var_11_7))
	else
		arg_11_0.elements:setLocalPosition(var_11_6, var_11_7)
	end

	if arg_11_0.listener ~= nil and arg_11_0.listener.onScroll ~= nil then
		arg_11_0.listener:onScroll(arg_11_0, arg_11_0.scrollXValue, arg_11_0.scrollYValue)
	end
end

function GUIScroll.scrollPercentX(arg_12_0, arg_12_1)
	local var_12_0, var_12_1 = arg_12_0.clickable:getBoxShapeSize()

	arg_12_0.scrollXValue = var_0_2(arg_12_1, 0, 1)

	if var_12_0 <= arg_12_0.layoutObj:getWidth() then
		arg_12_1 = 1 - arg_12_1
	end

	local var_12_2, var_12_3, var_12_4, var_12_5 = arg_12_0:getBoundaries()

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_12_0) .. "]\tGUIScroll:scrollXY() : clamp X = [" .. var_12_2 .. ":" .. var_12_3 .. "]")
	end

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_12_0) .. "]\tGUIScroll:scrollXY() : clamp Y = [" .. var_12_4 .. ":" .. var_12_5 .. "]")
	end

	local var_12_6 = arg_12_1 * (var_12_3 - var_12_2) + var_12_2

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_12_0) .. "]\tGUIScroll:scrollXY() : pos  = " .. tostring(var_12_6) .. "," .. tostring(y))
	end

	if GUI.roundedUnits then
		arg_12_0.elements:setLocalPositionX(var_0_1(var_12_6))
	else
		arg_12_0.elements:setLocalPositionX(var_12_6)
	end

	if arg_12_0.listener ~= nil and arg_12_0.listener.onScroll ~= nil then
		arg_12_0.listener:onScroll(arg_12_0, arg_12_0.scrollXValue, arg_12_0.scrollYValue)
	end
end

function GUIScroll.scrollPercentY(arg_13_0, arg_13_1)
	local var_13_0, var_13_1 = arg_13_0.clickable:getBoxShapeSize()

	arg_13_0.scrollYValue = var_0_2(arg_13_1, 0, 1)

	if var_13_1 >= arg_13_0.layoutObj:getHeight() then
		arg_13_1 = 1 - arg_13_1
	end

	local var_13_2, var_13_3, var_13_4, var_13_5 = arg_13_0:getBoundaries()

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_13_0) .. "]\tGUIScroll:scrollXY() : clamp X = [" .. var_13_2 .. ":" .. var_13_3 .. "]")
	end

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_13_0) .. "]\tGUIScroll:scrollXY() : clamp Y = [" .. var_13_4 .. ":" .. var_13_5 .. "]")
	end

	local var_13_6 = arg_13_1 * (var_13_5 - var_13_4) + var_13_4

	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_13_0) .. "]\tGUIScroll:scrollXY() : pos  = " .. tostring(x) .. "," .. tostring(var_13_6))
	end

	if GUI.roundedUnits then
		arg_13_0.elements:setLocalPositionY(var_0_1(var_13_6))
	else
		arg_13_0.elements:setLocalPositionY(var_13_6)
	end

	if arg_13_0.listener ~= nil and arg_13_0.listener.onScroll ~= nil then
		arg_13_0.listener:onScroll(arg_13_0, arg_13_0.scrollXValue, arg_13_0.scrollYValue)
	end
end

function GUIScroll.impulse(arg_14_0, arg_14_1, arg_14_2)
	if not arg_14_0.hasInnerScroll then
		local var_14_0, var_14_1 = arg_14_0.clickable:getBoxShapeSize()

		if var_0_0(arg_14_1) > 0 and var_14_0 > arg_14_0.layoutObj:getWidth() then
			return
		end

		if var_0_0(arg_14_2) > 0 and var_14_1 > arg_14_0.layoutObj:getHeight() then
			return
		end
	end

	arg_14_0.computeInertia = true
	arg_14_0.isDragging = false
	arg_14_0.isDragEngaged = false
	arg_14_0.speedX = arg_14_1
	arg_14_0.speedY = arg_14_2
end

function GUIScroll.getItemCount(arg_15_0)
	return arg_15_0.layoutObj:getItemCount()
end

function GUIScroll.getItem(arg_16_0, arg_16_1)
	return arg_16_0.layoutObj:getItem(arg_16_1)
end

function GUIScroll.setNodeSize(arg_17_0, arg_17_1, arg_17_2, arg_17_3)
	arg_17_0.layoutObj:setElementSize(arg_17_1, arg_17_2, arg_17_3)
end

function GUIScroll.insertNode(arg_18_0, arg_18_1, arg_18_2, arg_18_3, arg_18_4, arg_18_5, arg_18_6)
	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_18_0) .. "]\tGUISCroll:insertNode() : " .. tostring(arg_18_1))
	end

	arg_18_5 = arg_18_5 or 0.5
	arg_18_6 = arg_18_6 or 0.5

	arg_18_0.layoutObj:insertNode(arg_18_2, arg_18_1, arg_18_3, arg_18_4, arg_18_5, arg_18_6)
	arg_18_1:setNewParent(arg_18_0.elements)

	arg_18_1.preListener = arg_18_0

	if not arg_18_0.layoutObj.refreshSuspended then
		arg_18_0:scrollPercentXY(arg_18_0.scrollXValue, arg_18_0.scrollYValue)
	end
end

function GUIScroll.insertElement(arg_19_0, arg_19_1, arg_19_2)
	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_19_0) .. "]\tGUISCroll:insertElement() : " .. tostring(_node))
	end

	arg_19_1:setNewParent(arg_19_0.elements)
	arg_19_0.layoutObj:insertElement(arg_19_2, arg_19_1)

	arg_19_1.preListener = arg_19_0

	if not arg_19_0.layoutObj.refreshSuspended then
		arg_19_0:scrollPercentXY(arg_19_0.scrollXValue, arg_19_0.scrollYValue)
	end
end

function GUIScroll.pushBackNode(arg_20_0, arg_20_1, arg_20_2, arg_20_3, arg_20_4, arg_20_5)
	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_20_0) .. "]\tGUISCroll:pushBackNode() : " .. tostring(arg_20_1))
	end

	arg_20_4 = arg_20_4 or 0.5
	arg_20_5 = arg_20_5 or 0.5

	arg_20_0.layoutObj:pushBackNode(arg_20_1, arg_20_2, arg_20_3, arg_20_4, arg_20_5)
	arg_20_1:setNewParent(arg_20_0.elements)

	arg_20_1.preListener = arg_20_0

	if not arg_20_0.layoutObj.refreshSuspended then
		arg_20_0:scrollPercentXY(arg_20_0.scrollXValue, arg_20_0.scrollYValue)
	end
end

function GUIScroll.pushBackElement(arg_21_0, arg_21_1)
	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_21_0) .. "]\tGUISCroll:pushBackElement() : " .. tostring(_node))
	end

	arg_21_1:setNewParent(arg_21_0.elements)
	arg_21_0.layoutObj:pushBackElement(arg_21_1)

	arg_21_1.preListener = arg_21_0

	if not arg_21_0.layoutObj.refreshSuspended then
		arg_21_0:scrollPercentXY(arg_21_0.scrollXValue, arg_21_0.scrollYValue)
	end
end

function GUIScroll.removeNodeByIndex(arg_22_0, arg_22_1)
	if GUI.DEBUG_LEVEL.scroll_layout then
		GUI:debugPrint("[" .. tostring(arg_22_0) .. "]\tGUISCroll:removeNodeByIndex() : @ " .. tostring(arg_22_1))
	end

	arg_22_0.layoutObj:removeByIndex(arg_22_1)
	arg_22_0.elements:removeChildNode(node)

	if not arg_22_0.layoutObj.refreshSuspended then
		arg_22_0:scrollPercentXY(arg_22_0.scrollXValue, arg_22_0.scrollYValue)
	end
end

function GUIScroll.removeNode(arg_23_0, arg_23_1)
	arg_23_0.layoutObj:removeElement(arg_23_1)
	arg_23_0.elements:removeChildNode(arg_23_1)

	if not arg_23_0.layoutObj.refreshSuspended then
		arg_23_0:scrollPercentXY(arg_23_0.scrollXValue, arg_23_0.scrollYValue)
	end
end

function GUIScroll.setupPreListenerToSelf(arg_24_0, arg_24_1)
	local var_24_0 = GUI:getGUIElementsOnNode(arg_24_1)

	for iter_24_0 = 1, #var_24_0 do
		var_24_0[iter_24_0].preListener = arg_24_0
	end
end

function GUIScroll.clear(arg_25_0)
	arg_25_0.layoutObj:clear()
end

function GUIScroll.suspendLayout(arg_26_0)
	arg_26_0.layoutObj:suspend()
end

function GUIScroll.resumeLayout(arg_27_0)
	arg_27_0.layoutObj:resume()
end

function GUIScroll.refreshLayout(arg_28_0)
	arg_28_0.layoutObj:refresh()
end
