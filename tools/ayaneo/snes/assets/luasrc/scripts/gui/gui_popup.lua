require("gui.lua")

GUIPopup = class(WorldNode)

function GUIPopup.start(arg_1_0)
	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_1_0) .. "]\tGUIPopup:start()")
	end

	if not arg_1_0.layer then
		arg_1_0.layer = 0
	end

	if arg_1_0.ok_button then
		GUI:enforceListener(arg_1_0.ok_button, arg_1_0, "GUIPopup", "GUIButton")
	end

	if arg_1_0.cancel_button then
		GUI:enforceListener(arg_1_0.cancel_button, arg_1_0, "GUIPopup", "GUIButton")
	end

	if arg_1_0.yes_button then
		GUI:enforceListener(arg_1_0.yes_button, arg_1_0, "GUIPopup", "GUIButton")
	end

	if arg_1_0.no_button then
		GUI:enforceListener(arg_1_0.no_button, arg_1_0, "GUIPopup", "GUIButton")
	end

	if arg_1_0.blockingClickable == nil then
		if GUI.DEBUG_LEVEL.hit_test then
			GUI:debugPrint("[" .. tostring(arg_1_0) .. "]\tGUIPopup create blocking area")
		end

		arg_1_0.blockingClickable = addNewComponentToNode(arg_1_0, COMPONENT_TYPE_CLICKABLE)

		arg_1_0.blockingClickable:setLayer(arg_1_0.layer)
		arg_1_0.blockingClickable:setBoxShape(GUI.screenWidth, GUI.screenHeight)
	end

	GUI:focusElement(nil)
end

function GUIPopup.stop(arg_2_0)
	if GUI.DEBUG_LEVEL.runtime then
		GUI:debugPrint("[" .. tostring(arg_2_0) .. "]\tGUIPopup:stop()")
	end

	if arg_2_0.blockingClickable ~= nil then
		removeAndDestroyComponentFromNode(arg_2_0, arg_2_0.blockingClickable)
	end
end

function GUIPopup.close(arg_3_0, arg_3_1)
	arg_3_0:setEnabled(false)

	if arg_3_0.listener ~= nil then
		arg_3_0.listener:onPopupClose(arg_3_0, arg_3_1)
	end
end

function GUIPopup.onButtonClick(arg_4_0, arg_4_1)
	if GUI.DEBUG_LEVEL.callbacks then
		GUI:debugPrint("[" .. tostring(arg_4_0) .. "]\tGUIPopup:onButtonClick()")
	end

	if arg_4_1 == arg_4_0.ok_button then
		arg_4_0:close("ok")
	elseif arg_4_1 == arg_4_0.cancel_button then
		arg_4_0:close("cancel")
	elseif arg_4_1 == arg_4_0.yes_button then
		arg_4_0:close("yes")
	elseif arg_4_1 == arg_4_0.no_button then
		arg_4_0:close("no")
	end
end

function GUIPopup.disable(arg_5_0)
	if arg_5_0.ok_button then
		arg_5_0.ok_button:setState("disabled")
	end

	if arg_5_0.cancel_button then
		arg_5_0.cancel_button:setState("disabled")
	end

	if arg_5_0.yes_button then
		arg_5_0.yes_button:setState("disabled")
	end

	if arg_5_0.no_button then
		arg_5_0.no_button:setState("disabled")
	end
end

function GUIPopup.enable(arg_6_0)
	if arg_6_0.ok_button then
		arg_6_0.ok_button:setState("idle")
	end

	if arg_6_0.cancel_button then
		arg_6_0.cancel_button:setState("idle")
	end

	if arg_6_0.yes_button then
		arg_6_0.yes_button:setState("idle")
	end

	if arg_6_0.no_button then
		arg_6_0.no_button:setState("idle")
	end
end
