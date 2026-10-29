/**
 * @type {import('@react-native-community/cli-types').UserDependencyConfig}
 */
module.exports = {
  dependency: {
    platforms: {
      android: {
        // cmakeListsPath: 'generated/jni/CMakeLists.txt',
        cxxModuleCMakeListsModuleName: 'session-recorder-react-native',
        cxxModuleCMakeListsPath: 'CMakeLists.txt',
        cxxModuleHeaderName: 'SessionRecorderReactNativeImpl',

        packageImportPath:
          'import com.sessionrecorderreactnative.SessionRecorderReactNativePackage;',
        packageInstance:
          'new SessionRecorderReactNativePackage(BuildConfig.DEBUG)',
      },
    },
  },
};
