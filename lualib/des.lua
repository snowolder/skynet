local crypt = require "skynet.crypt"

local DES={}

function DES.encode(key,text)
{
	local s1=crypt.desencode(key,text)
	local s2=crypt.base64encode(s1)
	local s2s=crypt.url_safe(s2)
	return s2s
}

function DES.decode(key,text)
{
	local s3s=crypt.url_safe_back(text)
	local s3=crypt.base64decode(s3s)
	local s4=crypt.desdecode(key,s3)
	return s4
}

return DES