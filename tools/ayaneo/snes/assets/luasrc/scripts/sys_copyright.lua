require("/scripts/core/core.lua")
require("/scripts/app/clover_autocancel.lua")
-- Defines the global oss_licenses_text_lines (the Open Source Software notices), extracted from the
-- console's /usr/share/legal/licenses.lua LuaJIT bytecode. Without it the OSS tab is blank.
require("/scripts/legal/oss_licenses.lua")

sys_copyright = class(gui_container)

function sys_copyright.start(arg_1_0)
	gui_container.start(arg_1_0)
	arg_1_0:disable()

	local var_1_0 = arg_1_0.oss_text:getChildByName("text"):getComponent(LabelComponent)

	-- Hand the OSS lines to the text widget as a Lua table, NOT via setTextArray. setTextArray would
	-- round-trip the 9912 lines through the host bridge as a 544KB JSON blob that the Lua JSON parser
	-- re-parses when the Legal screen builds its text (~200ms, which froze the screen on open, unlike the
	-- device's instant open). Kept in Lua, sys_copyright_text reads it directly with no bridge crossing.
	arg_1_0.oss_text.textArrayLua = oss_licenses_text_lines
	var_1_0:setFontResource(sys_boot:getInstance().fonts.copyright)
end

function sys_copyright.update(arg_2_0, arg_2_1)
	if not GUI.disabled then
		if CloverPadUI.pressed.up or CloverPadUI.pressed.down then
			GUI.REPEAT_DELAY = 0.3
			GUI.REPEAT_RATE = 0.1
		elseif CloverPadUI.pressed.left or CloverPadUI.pressed.right then
			GUI.REPEAT_DELAY = 0.5
			GUI.REPEAT_RATE = 0.2
		end
	end

	gui_container.update(arg_2_0, arg_2_1)

	if arg_2_0.currentMenu ~= arg_2_0.current then
		arg_2_0.currentMenu = arg_2_0.current

		if arg_2_0.current:getName() == "copyright" then
			arg_2_0.oss_text:disable()
			arg_2_0.copyright_text:enable()
			arg_2_0.copyright_text:resetScroll()

			arg_2_0.currentText = arg_2_0.copyright_text
		else
			arg_2_0.copyright_text:disable()
			arg_2_0.oss_text:enable()
			arg_2_0.oss_text:resetScroll()

			arg_2_0.currentText = arg_2_0.oss_text
		end
	end

	if arg_2_0.currentText then
		if arg_2_0.currentText.userControl == false and not arg_2_0.currentText.arrowDownDisable then
			CloverAutoCancel.SetInterrupt("sys_copyright", true)
		else
			CloverAutoCancel.SetInterrupt("sys_copyright", false)
		end
	end
end

local function var_0_0()
	local var_3_0 = {}

	if not titles_list then
		return var_3_0
	end

	function tableSort(arg_4_0)
		local var_4_0 = {}
		local var_4_1 = 0

		for iter_4_0, iter_4_1 in pairs(arg_4_0) do
			var_4_1 = var_4_1 + 1
			var_4_0[var_4_1] = iter_4_0
		end

		table.sort(var_4_0, function(arg_5_0, arg_5_1)
			local var_5_0 = arg_4_0[arg_5_0].sort_raw_publisher
			local var_5_1 = arg_4_0[arg_5_1].sort_raw_publisher

			if var_5_0 ~= var_5_1 then
				return var_5_0 < var_5_1
			end

			local var_5_2 = arg_4_0[arg_5_0].release_date
			local var_5_3 = arg_4_0[arg_5_1].release_date

			if var_5_2 ~= var_5_3 then
				return var_5_2 < var_5_3
			end

			return arg_4_0[arg_5_0].sort_raw_title < arg_4_0[arg_5_1].sort_raw_title
		end)

		return var_4_0
	end

	local var_3_1 = tableSort(titles_list)
	local var_3_2 = {}

	for iter_3_0, iter_3_1 in pairs(var_3_1) do
		local var_3_3 = titles_list[iter_3_1]

		if var_3_3.copyright then
			local var_3_4 = var_3_3.copyright

			if not var_3_2[var_3_4] then
				var_3_0[#var_3_0 + 1] = var_3_4
				var_3_2[var_3_4] = true
			end
		end
	end

	local var_3_5

	if false then
		for iter_3_2, iter_3_3 in pairs(titles_list) do
			if iter_3_3.copyright then
				var_3_0[#var_3_0 + 1] = iter_3_3.sort_raw_publisher .. " : " .. iter_3_3.raw_title .. "(" .. iter_3_3.copyright .. ")"
			end
		end
	end

	return var_3_0
end

function sys_copyright.activate(arg_6_0)
	gui_container.activate(arg_6_0)
	gui_custom:popGUIStatus()
	gui_custom:pushGUIStatus("Copyright")
	GUI:focusElement(arg_6_0.current, true)

	if not arg_6_0.hasAddedCopyrightText then
		arg_6_0.hasAddedCopyrightText = true

		local var_6_0 = var_0_0(titles_list)

		if #var_6_0 > 0 then
			arg_6_0.copyright_text:setAdditionalTextFont(sys_boot:getInstance().fonts.copyright)
			arg_6_0.copyright_text:addAdditionalText(table.concat(var_6_0, "\n"))
		end
	end

	arg_6_0.oss_text:resetText()
	arg_6_0.copyright_text:resetText()
	arg_6_0.copyright_text:resetScroll()
	arg_6_0.oss_text:resetScroll()

	arg_6_0.currentMenu = nil

	CloverAutoCancel.Set("sys_copyright", arg_6_0)
end

function sys_copyright.onElementFocus(arg_7_0, arg_7_1)
	gui_container.onElementFocus(arg_7_0, arg_7_1)

	for iter_7_0, iter_7_1 in ipairs(arg_7_0.elementArray) do
		if iter_7_1.focused_node then
			if iter_7_1 == arg_7_1 then
				iter_7_1.focused_node:enable()
			else
				iter_7_1.focused_node:disable()
			end
		end
	end
end

function sys_copyright.onContainerCanceled(arg_8_0, arg_8_1)
	CloverAutoCancel.Set("sys_copyright", nil)
	Main:toHomeMenu()

	if arg_8_0.cancelSound then
		arg_8_0.cancelSound:stop()
		arg_8_0.cancelSound:play()
	end
end
