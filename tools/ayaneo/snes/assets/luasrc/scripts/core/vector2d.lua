local var_0_0 = math.sqrt

Vector2D = {}

function Vector2D.setLength(arg_1_0, arg_1_1, arg_1_2)
	local var_1_0 = Vector2D.length(arg_1_0, arg_1_1)

	return arg_1_0 * arg_1_2 / var_1_0, arg_1_1 * arg_1_2 / var_1_0
end

function Vector2D.length(arg_2_0, arg_2_1)
	return var_0_0(arg_2_0 * arg_2_0 + arg_2_1 * arg_2_1)
end

function Vector2D.sqLength(arg_3_0, arg_3_1)
	return arg_3_0 * arg_3_0 + arg_3_1 * arg_3_1
end

function Vector2D.normalize(arg_4_0, arg_4_1)
	local var_4_0 = var_0_0(arg_4_0 * arg_4_0 + arg_4_1 * arg_4_1)

	return arg_4_0 / var_4_0, arg_4_1 / var_4_0, var_4_0
end

function Vector2D.dotProduct(arg_5_0, arg_5_1, arg_5_2, arg_5_3)
	return arg_5_0 * arg_5_2 + arg_5_1 * arg_5_3
end
