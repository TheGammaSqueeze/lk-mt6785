require("/scripts/core/core.lua")

sys_option = class(gui_container)

function sys_option.activate(arg_1_0)
	arg_1_0:setVersionString()
	gui_container.activate(arg_1_0)
end

function sys_option.setVersionString(arg_2_0)
	if not arg_2_0.versionLabel then
		return
	end

	if arg_2_0.hasSetVersionLabel then
		return
	end

	arg_2_0.hasSetVersionLabel = true

	if not CLOVER_IS_DEBUG then
		arg_2_0.versionLabel:getNode():disable()

		return
	end

	if VERSION_PATH == nil then
		VERSION_PATH = {
			"/etc/issue",
			"/etc/clover/kachikachi",
			"/etc/clover/menu.final"
		}
	end

	function loadVersionString(arg_3_0)
		local var_3_0 = "CLOVER-UI MESSAGE rev." .. Localization.getText("REVISION")
		local var_3_1 = "menu rev. " .. FileUtils.readFile(Application.getDataPath("version")) .. "\n"

		if HOST_PLATFORM_IS_WINDOWS then
			return var_3_1 .. "Windows Simulator\n" .. var_3_0
		end

		if type(arg_3_0) == "string" then
			arg_3_0 = {
				arg_3_0
			}
		end

		local var_3_2 = ""

		for iter_3_0 = 1, #arg_3_0 do
			local var_3_3 = FileUtils.readFile(arg_3_0[iter_3_0])

			if var_3_3 ~= "" then
				var_3_2 = var_3_2 .. var_3_3 .. "\n"
			end
		end

		return var_3_1 .. var_3_2 .. var_3_0
	end

	local var_2_0 = loadVersionString(VERSION_PATH)

	arg_2_0.versionLabel:setText(var_2_0)
	arg_2_0.versionLabel:getNode():enable()
end
