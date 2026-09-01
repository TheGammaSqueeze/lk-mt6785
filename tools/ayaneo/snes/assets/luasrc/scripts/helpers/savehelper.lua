require("../core/core.lua")

SaveHelper = class()

function SaveHelper.saveInit(arg_1_0)
	arg_1_0.saveActionsWaiting = {}
end

function SaveHelper.createSaveFile(arg_2_0, arg_2_1)
	print("SaveHelper::createSaveFile " .. arg_2_1)

	return Save.create(arg_2_1, 512)
end

function SaveHelper.readSaveFile(arg_3_0, arg_3_1)
	local var_3_0 = coroutine.running()

	if not var_3_0 then
		error("SaveHelper:readSaveFile : Must be called inside a coroutine")
	end

	arg_3_0.saveActionsWaiting[arg_3_1] = var_3_0

	Save.read(arg_3_0._ptr, arg_3_1)

	return coroutine.yield()
end

function SaveHelper.writeSaveFile(arg_4_0, arg_4_1, arg_4_2)
	local var_4_0 = coroutine.running()

	if not var_4_0 then
		error("SaveHelper:writeSaveFile : Must be called inside a coroutine")
	end

	arg_4_0.saveActionsWaiting[arg_4_1] = var_4_0

	Save.write(arg_4_0._ptr, arg_4_1, arg_4_2)

	return coroutine.yield()
end

function SaveHelper.deleteSaveFile(arg_5_0, arg_5_1)
	local var_5_0 = coroutine.running()

	if not var_5_0 then
		error("SaveHelper:deleteSaveFile : Must be called inside a coroutine")
	end

	arg_5_0.saveActionsWaiting[arg_5_1] = var_5_0

	Save.delete(arg_5_0._ptr, arg_5_1)

	return coroutine.yield()
end

function SaveHelper.closeSaveFile(arg_6_0, arg_6_1)
	return Save.release(arg_6_1)
end

function SaveHelper.onSaveActionDone(arg_7_0, arg_7_1, arg_7_2, arg_7_3, arg_7_4)
	local var_7_0, var_7_1 = coroutine.resume(arg_7_0.saveActionsWaiting[arg_7_1], arg_7_2, arg_7_3)

	if var_7_0 == false then
		error(tostring(var_7_1))
	end
end
