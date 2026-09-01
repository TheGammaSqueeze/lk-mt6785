require("gui_element.lua")

GUIRadioGroup = class(GUIElement)

function GUIRadioGroup.start(arg_1_0)
	arg_1_0.radiobuttons = {}

	local var_1_0 = 1
	local var_1_1 = arg_1_0:getFirstChildNode()

	while var_1_1 do
		if var_1_1:isInstanceOf(GUICheckButton) then
			arg_1_0.radiobuttons[var_1_0] = var_1_1
			var_1_1.listener = arg_1_0
			var_1_1.radioMode = true

			if var_1_0 == arg_1_0.selected then
				var_1_1:setChecked(true)
			else
				var_1_1:setChecked(false)
			end

			var_1_0 = var_1_0 + 1
		end

		var_1_1 = var_1_1:getSiblingNode()
	end
end

function GUIRadioGroup.addRadioButton(arg_2_0, arg_2_1)
	if arg_2_1:isInstanceOf(GUICheckButton) then
		local var_2_0 = arg_2_0.radiobuttons

		var_2_0[#var_2_0 + 1] = arg_2_1
		arg_2_1.listener = arg_2_0
		arg_2_1.radioMode = true

		arg_2_1:setChecked(false)
	end
end

function GUIRadioGroup.removeRadioButton(arg_3_0, arg_3_1)
	local var_3_0 = 0
	local var_3_1 = arg_3_0.radiobuttons

	for iter_3_0 = 1, #var_3_1 do
		if var_3_1[iter_3_0] == arg_3_1 then
			table.remove(var_3_1, iter_3_0)

			break
		end
	end
end

function GUIRadioGroup.onCheckToggle(arg_4_0, arg_4_1, arg_4_2)
	if arg_4_2 then
		for iter_4_0 = 1, #arg_4_0.radiobuttons do
			local var_4_0 = arg_4_0.radiobuttons[iter_4_0]

			if var_4_0 ~= arg_4_1 then
				var_4_0:setChecked(false)
			else
				arg_4_0.selected = iter_4_0
			end
		end

		if arg_4_0.listener and arg_4_0.listener.onRadioSelectionChanged then
			arg_4_0.listener:onRadioSelectionChanged(arg_4_0, arg_4_0.selected)
		end
	end
end

function GUIRadioGroup.getSelectedIndex(arg_5_0)
	return arg_5_0.selected
end

function GUIRadioGroup.getSelectedButton(arg_6_0)
	return arg_6_0.radiobuttons[arg_6_0.selected]
end

function GUIRadioGroup.getButtonCount(arg_7_0)
	return #arg_7_0.radiobuttons
end

function GUIRadioGroup.selectButton(arg_8_0, arg_8_1)
	-- a parent controller can call this from its own start() before this radiogroup's
	-- start() has populated radiobuttons (child controllers start after their parent),
	-- so build it on demand (start is idempotent; the button nodes already exist).
	if not arg_8_0.radiobuttons then
		arg_8_0:start()
	end

	for iter_8_0 = 1, #arg_8_0.radiobuttons do
		local var_8_0 = arg_8_0.radiobuttons[iter_8_0]

		if var_8_0 ~= arg_8_1 then
			var_8_0:setChecked(false)
		else
			var_8_0:setChecked(true)

			arg_8_0.selected = iter_8_0
		end
	end
end

function GUIRadioGroup.selectIndex(arg_9_0, arg_9_1)
	if not arg_9_0.radiobuttons then
		arg_9_0:start()
	end

	for iter_9_0 = 1, #arg_9_0.radiobuttons do
		local var_9_0 = arg_9_0.radiobuttons[iter_9_0]

		if arg_9_1 ~= iter_9_0 then
			var_9_0:setChecked(false)
		else
			var_9_0:setChecked(true)

			arg_9_0.selected = iter_9_0
		end
	end
end
