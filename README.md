## 功能 Features
本工具用于在win系统上，支持的功能有：
- 获取连接的手机设备列表
- 对ipa用Apple免费账号重签名
- 对ipa用Apple自有证书重签名

## 使用步骤 Usage
1. 下载[MiniAppBuilder](https://github.com/yujon/ipa-win-builder/releases/)并解压

2. 将`iphone`手机用数据线连接到电脑上

3. 然后打开终端工具，执行以下命令，该命令将会从ipa重签名并安装到手机上

```sh
cd MiniappMacBuilder-xxx
# 获取连接的设备列表信息
# ./MiniAppBuilder.exe --action getDevices 

# 免费证书签名
./MiniAppBuilder.exe --action sign --type appleId --ipa {ipaPath} --install true
./MiniAppBuilder.exe --action sign --type appleId --ipa {ipaPath} --appleId xxx --password xxx --install true
# 自有证书签名
./MiniAppBuilder.exe --action sign --type certificate --ipa {ipaPath} --certificatePath xxx --certificatePassword xxx --profilePath xxx --install true
# 导出ipa
./MiniAppBuilder.exe --action sign --type appleId --ipa {ipaPath} --export /aaa/bbb/ccc
# 指定bundleId(默认为same、auto代表自动分配（在现有bundleId后加{.teamId}、xxxx是自定义的值)
./MiniAppBuilder.exe --action sign --type appleId --ipa {ipaPath} --bundleId same|auto|xxxx --install true
# 指定entitlements(格式为A=xx&B=xxx，设置的每一项应该是bundleId已经具备的权限，否则会被过滤)
./MiniAppBuilder.exe --action sign --type appleId --ipa {ipaPath} com.apple.developer.associated-domains=htpps://www.test.com/a/ --install true

# 清除緩存（Remember之类）
# ./MiniAppBuilder.exe --action clear 
```

4. 免费证书签名是，如果用的apple账号与手机登录的不同，需要到手机端打开：设置 -> 通用 -> VPN与设备管理，然后选择信任对应的签名apple账号

## 常见问题 QA
