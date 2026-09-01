require("gui.lua")

GUIList = class(GUIElement)

function GUIList.start(arg_1_0)
	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_1_0) .. "]\tGUIList:start()")
	end

	if not arg_1_0.scroll then
		GUI:error("GUIList must have a reference to a GUIScroll element")

		return
	end

	arg_1_0:enforceListenerToSelf(arg_1_0.scroll, "GUIList", "GUIScroll")

	arg_1_0.scroll.preListener = arg_1_0

	if arg_1_0.scrollBarH then
		arg_1_0.scrollBarH.direction = "horizontal"

		arg_1_0:enforceListenerToSelf(arg_1_0.scrollBarH, "GUIList", "GUISlider")

		arg_1_0.scrollBarH.preListener = arg_1_0

		arg_1_0.scrollBarH:setValue(1 - arg_1_0.scroll.scrollXValue)
	end

	if arg_1_0.scrollBarV then
		arg_1_0.scrollBarV.direction = "vertical"

		arg_1_0:enforceListenerToSelf(arg_1_0.scrollBarV, "GUIList", "GUISlider")

		arg_1_0.scrollBarV.preListener = arg_1_0

		arg_1_0.scrollBarV:setValue(1 - arg_1_0.scroll.scrollYValue)
	end

	GUIElement.start(arg_1_0)
end

function GUIList.onSliderPressed(arg_2_0, arg_2_1, arg_2_2)
	arg_2_0.scroll:stopScroll()
end

function GUIList.onSliderValueChange(arg_3_0, arg_3_1, arg_3_2)
	if arg_3_1 == arg_3_0.scrollBarH then
		arg_3_0.scroll:scrollPercentX(1 - arg_3_2)
	elseif arg_3_1 == arg_3_0.scrollBarV then
		arg_3_0.scroll:scrollPercentY(1 - arg_3_2)
	end
end

function GUIList.onScroll(arg_4_0, arg_4_1, arg_4_2, arg_4_3)
	if arg_4_1 == arg_4_0.scroll then
		if arg_4_0.scrollBarH then
			arg_4_0.scrollBarH:setValue(1 - arg_4_2)
		end

		if arg_4_0.scrollBarV then
			arg_4_0.scrollBarV:setValue(1 - arg_4_3)
		end
	end
end

function GUIList.setScrollValues(arg_5_0, arg_5_1, arg_5_2)
	if arg_5_0.scrollBarH then
		arg_5_0:setScrollX(arg_5_1)
	end

	if arg_5_0.scrollBarV then
		arg_5_0:setScrollY(arg_5_2)
	end
end

function GUIList.getScrollX(arg_6_0)
	return arg_6_0.scrollBarH.currentValue
end

function GUIList.setScrollX(arg_7_0, arg_7_1)
	arg_7_0.scrollBarH:setValue(arg_7_1)
	arg_7_0.scroll:scrollPercentX(1 - arg_7_1)
end

function GUIList.getScrollY(arg_8_0)
	return arg_8_0.scrollBarV.currentValue
end

function GUIList.setScrollY(arg_9_0, arg_9_1)
	arg_9_0.scrollBarV:setValue(arg_9_1)
	arg_9_0.scroll:scrollPercentY(1 - arg_9_1)
end
