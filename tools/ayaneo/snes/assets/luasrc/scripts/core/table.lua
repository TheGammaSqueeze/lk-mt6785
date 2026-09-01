function printTable(arg_1_0, arg_1_1)
	if arg_1_1 == nil then
		arg_1_1 = 0
	end

	local var_1_0 = string.rep("    ", arg_1_1 + 1)
	local var_1_1 = string.rep("    ", arg_1_1)

	print(var_1_1 .. "{")

	for iter_1_0, iter_1_1 in pairs(arg_1_0) do
		if type(iter_1_1) == "table" then
			if type(iter_1_0) == "string" then
				print(var_1_0 .. tostring(iter_1_0) .. " = ")
			end

			printTable(iter_1_1, arg_1_1 + 1)
		else
			print(var_1_0 .. tostring(iter_1_0) .. " = " .. tostring(iter_1_1))
		end
	end

	print(var_1_1 .. "}")
end
