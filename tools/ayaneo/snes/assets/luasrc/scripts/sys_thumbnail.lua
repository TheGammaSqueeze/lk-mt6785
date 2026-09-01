require("/scripts/core/core.lua")

sys_thumbnail = class(WorldNode)

function sys_thumbnail.update(arg_1_0, arg_1_1)
	if arg_1_0.loading and arg_1_0.info and arg_1_0.info.iconResource:isLoaded() then
		arg_1_0:loadingDone()

		arg_1_0.loading = nil
		system.loadingCount_thumbnail = system.loadingCount_thumbnail - 1
	end
end

function sys_thumbnail.setInfo(arg_2_0, arg_2_1)
	arg_2_0.info = arg_2_1

	if arg_2_0.info.iconResource:isLoading() then
		arg_2_0.loading = true
		system.loadingCount_thumbnail = system.loadingCount_thumbnail + 1
	elseif arg_2_0.info.iconResource:isLoaded() then
		arg_2_0:loadingDone()
	end
end

function sys_thumbnail.loadingDone(arg_3_0)
	local var_3_0, var_3_1 = arg_3_0.info.iconResource:getSize()

	arg_3_0.component:setTexture(arg_3_0.info.iconResource)
	arg_3_0.component:setSize(var_3_0, var_3_1)
	arg_3_0.component:setPivot(0.5, 1)
end
