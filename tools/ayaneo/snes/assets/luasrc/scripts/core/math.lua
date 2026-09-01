Math = {}

function Math.lerp(arg_1_0, arg_1_1, arg_1_2)
	return arg_1_0 + (arg_1_1 - arg_1_0) * arg_1_2
end

function Math.clamp(arg_2_0, arg_2_1, arg_2_2)
	if arg_2_0 <= arg_2_1 then
		return arg_2_1
	elseif arg_2_2 <= arg_2_0 then
		return arg_2_2
	else
		return arg_2_0
	end
end

function Math.round(arg_3_0, arg_3_1)
	local var_3_0 = 10^(arg_3_1 or 0)

	return math.floor(arg_3_0 * var_3_0 + 0.5) / var_3_0
end
