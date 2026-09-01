require("/scripts/core/core.lua")

dot_number = class(WorldNode)

function dot_number.make_spriteTable(arg_1_0)
	arg_1_0.spriteTable = {
		[0] = arg_1_0["0"],
		arg_1_0["1"],
		arg_1_0["2"],
		arg_1_0["3"],
		arg_1_0["4"],
		arg_1_0["5"],
		arg_1_0["6"],
		arg_1_0["7"],
		arg_1_0["8"],
		arg_1_0["9"]
	}
end

function dot_number.setNumber(arg_2_0, arg_2_1)
	if not arg_2_0.spriteTable then
		arg_2_0:make_spriteTable()
	end

	local var_2_0 = math.floor(arg_2_1 / 60) % 60
	local var_2_1 = math.floor(arg_2_1 / 60 / 60)

	if var_2_1 > 99 then
		var_2_1 = 99
		var_2_0 = 59
	end

	local var_2_2 = var_2_0 % 10
	local var_2_3 = math.floor(var_2_0 / 10)
	local var_2_4 = var_2_1 % 10
	local var_2_5 = math.floor(var_2_1 / 10)

	arg_2_0.min1:setSprite(arg_2_0.spriteTable[var_2_2])
	arg_2_0.min2:setSprite(arg_2_0.spriteTable[var_2_3])
	arg_2_0.hour1:setSprite(arg_2_0.spriteTable[var_2_4])
	arg_2_0.hour2:setSprite(arg_2_0.spriteTable[var_2_5])

	if var_2_1 < 10 then
		arg_2_0.hour2:disable()
	else
		arg_2_0.hour2:enable()
	end
end
