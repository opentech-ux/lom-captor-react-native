require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "SessionRecorderReactNative"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => min_ios_version_supported }
  s.source       = { :git => "https://github.com/opentech-ux/lom-captor-react-native.git", :tag => "#{s.version}" }

  s.source_files = "ios/**/*.{h,m,mm}", "cpp/**/*.{hpp,cpp,c,h}", "ios/generated/*.{h,cpp,mm}"
  s.private_header_files = "ios/**/*.h", "cpp/**/*.{h,hpp}"
  s.frameworks = "WebKit"
  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => '$(inherited) "$(PODS_TARGET_SRCROOT)/cpp"'
  }

  install_modules_dependencies(s)
end
