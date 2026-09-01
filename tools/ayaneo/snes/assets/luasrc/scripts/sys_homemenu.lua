require("/scripts/core/core.lua")
require("/scripts/system.lua")

sys_homemenu = class(gui_container)

function sys_homemenu.hudSelect(arg_1_0, arg_1_1)
	local var_1_0 = {
		gametitle = arg_1_0.hud_gametitle,
		menubar = arg_1_0.hud_menubar
	}

	for iter_1_0, iter_1_1 in pairs(var_1_0) do
		if iter_1_0 == arg_1_1 then
			iter_1_1:enable()
		else
			iter_1_1:disable()
		end
	end
end

function sys_homemenu.deactivate(arg_2_0)
	gui_container.deactivate(arg_2_0)
	-- CLOVER keeps the home scene rendered while the opaque submenu slides in over it
	-- (deactivate gates input, not rendering); it is covered once the submenu settles. Without
	-- this the home HUD vanishes instantly and the submenu enter shows bare wallpaper.
	Host.setEnabled(arg_2_0, true)
end
