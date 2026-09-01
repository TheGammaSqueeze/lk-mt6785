function iterate_children(arg_1_0)
	return function(arg_2_0, arg_2_1)
		if arg_2_1 == nil then
			return arg_2_0:getFirstChildNode()
		else
			return arg_2_1:getSiblingNode()
		end
	end, arg_1_0, nil
end

function getObjects(arg_3_0, arg_3_1)
	local var_3_0 = {}

	for iter_3_0 in iterate_children(arg_3_0) do
		if iter_3_0:isInstanceOf(arg_3_1) then
			table.insert(var_3_0, iter_3_0)

			if iter_3_0.hvc_only and not system.is_hvc() or iter_3_0.nes_only and not system.is_nes() then
				iter_3_0:disable()
			end
		end
	end

	return var_3_0
end

function sort_positionComp(arg_4_0, arg_4_1)
	local var_4_0, var_4_1 = arg_4_0:getLocalPosition()
	local var_4_2, var_4_3 = arg_4_1:getLocalPosition()

	if var_4_1 == var_4_3 then
		return var_4_0 < var_4_2
	else
		return var_4_3 < var_4_1
	end
end

function sort_positionCompV(arg_5_0, arg_5_1)
	local var_5_0, var_5_1 = arg_5_0:getLocalPosition()
	local var_5_2, var_5_3 = arg_5_1:getLocalPosition()

	if var_5_0 == var_5_2 then
		return var_5_3 < var_5_1
	else
		return var_5_0 < var_5_2
	end
end

function elementIsEnabled(arg_6_0)
	return arg_6_0 and arg_6_0:isEnabled() and arg_6_0.currentState ~= "disabled"
end
