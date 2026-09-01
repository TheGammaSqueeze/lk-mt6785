require("/scripts/core/core.lua")
require("/scripts/helper_index.lua")
require("/scripts/helper_nodes.lua")
require("/scripts/app/clover_elements.lua")

sys_menubarU = class(gui_container)

function sys_menubarU.start(arg_1_0)
	gui_container.start(arg_1_0)

	if system.is_hvc() then
		if arg_1_0.hvc_position then
			arg_1_0.hvc_position:change()
		end

		if arg_1_0.bar_hvc then
			arg_1_0.bar_hvc:enable()
		end

		if arg_1_0.bar_nes then
			arg_1_0.bar_nes:disable()
		end
	elseif system.is_nes() then
		if arg_1_0.bar_hvc then
			arg_1_0.bar_hvc:disable()
		end

		if arg_1_0.bar_nes then
			arg_1_0.bar_nes:enable()
		end
	end
end

function sys_menubarU.activate(arg_2_0)
	gui_container.activate(arg_2_0)

	if arg_2_0.active_position then
		arg_2_0.active_position:changeAnimation()
	end

	if arg_2_0.menu then
		arg_2_0.menu:hudSelect("menubar")
	end

	CloverElements.resumedummy:focusMenubar()
end

function sys_menubarU.deactivate(arg_3_0)
	gui_container.deactivate(arg_3_0)
	CloverElements.gametitlelist:setNextCursorZ(15)

	if arg_3_0.active_position then
		arg_3_0.active_position:revertAnimation()
	end

	-- The top filmstrip bar and its five icons stay visible on the home menu whether the carousel
	-- or the menubar is focused (deactivate gates input, not rendering). gui_container.deactivate
	-- disabled the whole subtree, so moving focus back down to the carousel made the bar vanish to
	-- black. Re-enable rendering; focus/decoration is handled per-icon by sys_menubar_cm.
	Host.setEnabled(arg_3_0, true)
end

function sys_menubarU.onButtonClick(arg_4_0, arg_4_1)
	local var_4_0 = table_find(arg_4_0.elementArray, arg_4_1)

	if var_4_0 then
		if var_4_0 == 1 then
			Main:toDisplay()
		elseif var_4_0 == 2 then
			Main:toSettings()
		elseif var_4_0 == 3 then
			Main:toLanguages()
		elseif var_4_0 == 4 then
			Main:toCopyRight()
		elseif var_4_0 == 5 then
			Main:toManual()
		end
	end
end

function sys_menubarU.onContainerCanceled(arg_5_0, arg_5_1)
	GUI:focusElement(arg_5_0.downElement)
	arg_5_0:deactivate()
end
