require("/scripts/core/core.lua")
require("/scripts/app/clover_autocancel.lua")

sys_manual = class(gui_container)

function sys_manual.start(arg_1_0)
	gui_container.start(arg_1_0)

	if hvcNode and nesNode then
		if system.is_hvc() then
			arg_1_0.hvcNode:enable()
			arg_1_0.nesNode:disable()
		elseif system.is_nes() then
			arg_1_0.hvcNode:disable()
			arg_1_0.nesNode:enable()
		end
	end
end

function sys_manual.activate(arg_2_0)
	gui_container.activate(arg_2_0)
	CloverAutoCancel.Set("sys_manual", arg_2_0)
end

function sys_manual.onContainerCanceled(arg_3_0, arg_3_1)
	CloverAutoCancel.Set("sys_manual", nil)
	Main:toHomeMenu()

	if arg_3_0.cancelSound then
		arg_3_0.cancelSound:stop()
		arg_3_0.cancelSound:play()
	end
end
