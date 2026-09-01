require("/scripts/core/core.lua")
require("/scripts/app/clover_autocancel.lua")

sys_option_languages = class(gui_container)

local var_0_0 = {
	{
		language = "en",
		caption = "English",
		region = PLATFORM_REGION_AMERICA
	},
	{
		language = "fr",
		caption = "French",
		region = PLATFORM_REGION_EUROPE
	},
	{
		language = "de",
		caption = "German",
		region = PLATFORM_REGION_EUROPE
	},
	{
		language = "it",
		caption = "Italian",
		region = PLATFORM_REGION_EUROPE
	},
	{
		language = "es",
		caption = "Spanish",
		region = PLATFORM_REGION_EUROPE
	},
	{
		language = "nl",
		caption = "Dutch",
		region = PLATFORM_REGION_EUROPE
	},
	{
		language = "pt",
		caption = "Portuguese",
		region = PLATFORM_REGION_EUROPE
	},
	{
		language = "ru",
		caption = "Russian",
		region = PLATFORM_REGION_EUROPE
	}
}

function sys_option_languages.start(arg_1_0)
	gui_container.start(arg_1_0)

	if HOST_PLATFORM_IS_WINDOWS then
		local var_1_0 = table_find_if(var_0_0, function(arg_2_0)
			return arg_2_0.caption == debug_store.language
		end)

		if var_1_0 then
			local var_1_1 = var_0_0[var_1_0]

			system.setLocale(var_1_1.language, "", var_1_1.region)
		end
	end

	local var_1_2, var_1_3, var_1_4 = Localization.getLocale()

	if system.is_nes() then
		if store.language_selected then
			local var_1_5
			local var_1_6
			local var_1_7 = table_find_if(var_0_0, function(arg_3_0)
				return arg_3_0.language == var_1_2 and arg_3_0.region == var_1_4
			end)

			if var_1_7 then
				var_1_5 = var_0_0[var_1_7]
				var_1_6 = table_find_if(arg_1_0.elementArray, function(arg_4_0)
					return arg_4_0.language == var_1_5.caption
				end)
			end

			if var_1_6 then
				local var_1_8 = arg_1_0.elementArray[var_1_6]

				arg_1_0.radiogroup:selectButton(var_1_8)

				arg_1_0.current = var_1_8
			else
				store.language_selected = false

				local var_1_9 = arg_1_0.elementArray[1]

				arg_1_0.radiogroup:selectButton(var_1_9)

				arg_1_0.current = var_1_9

				local var_1_10 = var_0_0[1]

				system.setLocale(var_1_10.language, "", var_1_10.region)
			end
		else
			local var_1_11 = arg_1_0.elementArray[1]

			arg_1_0.radiogroup:selectButton(var_1_11)

			arg_1_0.current = var_1_11

			local var_1_12 = var_0_0[1]

			system.setLocale(var_1_12.language, "", var_1_12.region)
		end
	else
		system.setLocale("ja", "", PLATFORM_REGION_JAPAN)
	end
end

function sys_option_languages.activate(arg_5_0)
	local var_5_0, var_5_1, var_5_2 = Localization.getLocale()

	if not arg_5_0.isFirstMode then
		local var_5_3 = table_find_if(var_0_0, function(arg_6_0)
			return arg_6_0.language == var_5_0 and arg_6_0.region == var_5_2
		end)
		local var_5_4 = var_0_0[var_5_3]
		local var_5_5 = table_find_if(arg_5_0.elementArray, function(arg_7_0)
			return arg_7_0.language == var_5_4.caption
		end)

		if var_5_5 then
			arg_5_0.current = arg_5_0.elementArray[var_5_5]
		else
			arg_5_0.current = arg_5_0.elementArray[1]
		end

		arg_5_0.radiogroup:selectButton(arg_5_0.current)
	end

	gui_container.activate(arg_5_0)

	if not arg_5_0.isFirstMode then
		CloverAutoCancel.Set("sys_option_languages", arg_5_0)
	end
end

function sys_option_languages.onRadioSelectionChanged(arg_8_0, arg_8_1)
	local var_8_0 = arg_8_0.radiogroup:getSelectedButton()
	local var_8_1 = table_find_if(var_0_0, function(arg_9_0)
		return arg_9_0.caption == var_8_0.language
	end)

	debug_store.language = var_0_0[var_8_1].caption
	arg_8_0.current = var_8_0
end

function sys_option_languages.onContainerCanceled(arg_10_0, arg_10_1)
	if not arg_10_0.isFirstMode then
		CloverAutoCancel.Set("sys_option_languages", nil)
		Main:toHomeMenu()

		if arg_10_0.cancelSound then
			arg_10_0.cancelSound:stop()
			arg_10_0.cancelSound:play()
		end
	end
end

function sys_option_languages.onElementCommand(arg_11_0, arg_11_1, arg_11_2)
	return gui_container.onElementCommand(arg_11_0, arg_11_1, arg_11_2)
end

function sys_option_languages.onElementFocus(arg_12_0, arg_12_1)
	gui_container.onElementFocus(arg_12_0, arg_12_1)

	if arg_12_1 and arg_12_1 ~= arg_12_0 then
		local var_12_0 = table_find(arg_12_0.elementArray, arg_12_1)

		if arg_12_0.isFirstMode and arg_12_0.decideButton then
			if var_12_0 == 5 or var_12_0 == 10 then
				arg_12_0.disabledVertical = true

				arg_12_0.decideButton:activate()
			else
				arg_12_0.disabledVertical = false

				arg_12_0.decideButton:deactivate()
			end
		end
	end
end

function sys_option_languages.onButtonClick(arg_13_0, arg_13_1)
	if arg_13_1 and arg_13_1 ~= arg_13_0 then
		local var_13_0 = table_find(arg_13_0.elementArray, arg_13_1)

		if arg_13_0.isFirstMode then
			if var_13_0 == 5 or var_13_0 == 10 then
				store.language_selected = true

				Main:toHomeMenu()

				return
			elseif var_13_0 >= 5 then
				var_13_0 = var_13_0 - 1
			end
		end

		local var_13_1 = arg_13_0.radiogroup:getSelectedButton()

		if arg_13_0.isFirstMode or var_13_1 == arg_13_1 then
			local var_13_2 = table_find_if(var_0_0, function(arg_14_0)
				return arg_14_0.caption == var_13_1.language
			end)
			local var_13_3 = var_0_0[var_13_2]

			system.setLocale(var_13_3.language, "", var_13_3.region)

			if arg_13_0.isFirstMode then
				if var_13_0 < 5 then
					GUI:focusElement(arg_13_0.elementArray[5], true)
				else
					GUI:focusElement(arg_13_0.elementArray[10], true)
				end
			end
		end
	end
end

function sys_option_languages.onCheckToggle(arg_15_0, arg_15_1, arg_15_2)
	arg_15_0.radiogroup:onCheckToggle(arg_15_1, arg_15_2)
end
