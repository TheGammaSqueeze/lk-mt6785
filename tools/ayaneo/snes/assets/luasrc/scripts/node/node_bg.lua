require("/scripts/core/core.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_util.lua")
require("/scripts/tween/tween_manager.lua")
require("/scripts/tween/tween_ease.lua")
require("/scripts/tween/tween.lua")

NodeBG = class(WorldNode)

function NodeBG.start(arg_1_0)
	CloverSingleton.Bind(NodeBG, arg_1_0)

	local var_1_0 = 0.625

	arg_1_0.current = 1
	arg_1_0.color_tbl = {
		CloverColor.new(var_1_0, 0, 0, 1),
		CloverColor.new(0, var_1_0, 0, 1),
		CloverColor.new(0, 0, var_1_0, 1),
		CloverColor.new(var_1_0, var_1_0, 0, 1),
		CloverColor.new(var_1_0, 0, var_1_0, 1),
		CloverColor.new(0, var_1_0, var_1_0, 1),
		CloverColor.black
	}
	arg_1_0.colorInterp = nil
end

function NodeBG.stop(arg_2_0)
	CloverSingleton.Reset(NodeBG)
end

function NodeBG.update(arg_3_0, arg_3_1)
	if not arg_3_0.colorInterp or not TweenManager:isTweenActive(arg_3_0.colorInterp) then
		local var_3_0 = arg_3_0.color_tbl[arg_3_0.current]

		arg_3_0.colorInterp = Tween:sequence(Tween:colorTo(arg_3_0.bg, 3, var_3_0.r, var_3_0.g, var_3_0.b, var_3_0.a, Ease.linear), Tween:wait(1)):start()
		arg_3_0.current = arg_3_0.current + 1

		if arg_3_0.current > #arg_3_0.color_tbl then
			arg_3_0.current = 1
		end
	end
end
