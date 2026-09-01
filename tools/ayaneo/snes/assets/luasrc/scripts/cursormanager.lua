require("/scripts/core/core.lua")
require("/scripts/system.lua")

CursorManager = class(WorldNode)

function CursorManager.start(arg_1_0)
	system.cursor = arg_1_0
	arg_1_0.cursors = {
		square = arg_1_0.cursor_square,
		rounded = arg_1_0.cursor_rounded
	}
	arg_1_0.current = nil
	arg_1_0.requestedStartZ = 0
end

function CursorManager.setCursor(arg_2_0, arg_2_1, arg_2_2, arg_2_3)
	local var_2_0 = arg_2_0.current
	local var_2_1 = arg_2_0.cursors[arg_2_1]

	if var_2_0 ~= var_2_1 then
		if not var_2_0 or not var_2_0:isEnabled() then
			arg_2_3 = true
		end

		if not arg_2_3 and var_2_0.target then
			var_2_1:moveToComponent(var_2_0.target, true)
		end

		if var_2_0 then
			var_2_0:disable()
		end

		var_2_1:enable()
	end

	var_2_1:moveToComponent(arg_2_2, arg_2_3, arg_2_0.requestedStartZ)

	arg_2_0.current = var_2_1
end

function CursorManager.setRounded(arg_3_0, arg_3_1, arg_3_2)
	arg_3_0:setCursor("rounded", arg_3_1, arg_3_2)
end

function CursorManager.setSquare(arg_4_0, arg_4_1, arg_4_2)
	arg_4_0:setCursor("square", arg_4_1, arg_4_2)
end

function CursorManager.cursorHide(arg_5_0)
	for iter_5_0, iter_5_1 in pairs(arg_5_0.cursors) do
		iter_5_1:disable()
	end

	arg_5_0.current = nil
end

function CursorManager.setNextStartZ(arg_6_0, arg_6_1)
	arg_6_0.requestedStartZ = arg_6_1
end
