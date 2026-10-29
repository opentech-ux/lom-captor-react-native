import { useState } from 'react';
import {
  Button,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  View,
} from 'react-native';

import { SessionRecorder } from 'lom-captor-react-native';

SessionRecorder.init({
  endpoint: 'https://demo-client.ux-key.com/endpoint',
  debugLog: true,
  debugSendSession: true,
});

export default function App() {
  const [count, setCount] = useState(0);
  const [isToggled, setIsToggled] = useState(false);
  const [text, setText] = useState('');

  return (
    <View style={styles.container}>
      <View style={styles.headerBox}>
        <Text style={styles.title}>Lab LOM</Text>
      </View>

      <View style={styles.interactiveBox}>
        <Text style={styles.counterText}>Count: {count}</Text>

        <Button title="Add (+1) " onPress={() => setCount((c) => c + 1)} />
      </View>

      <View style={styles.row}>
        <Text>Show hidden box:</Text>
        <Switch value={isToggled} onValueChange={(val) => setIsToggled(val)} />
      </View>

      {isToggled && (
        <View style={styles.hiddenBox}>
          <Text style={styles.subtitle}>NATIVE TREE!</Text>
        </View>
      )}

      <TextInput
        style={styles.input}
        placeholder="Write here..."
        value={text}
        onChangeText={setText}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#f5f5f5',
    paddingTop: 60,
    alignItems: 'center',
  },
  headerBox: {
    width: '90%',
    padding: 20,
    backgroundColor: '#fff',
    borderRadius: 10,
    shadowColor: '#000',
    shadowOpacity: 0.1,
    shadowRadius: 5,
    elevation: 3,
    marginBottom: 20,
    alignItems: 'center',
  },
  title: {
    fontSize: 22,
    fontWeight: 'bold',
  },
  subtitle: {
    fontSize: 14,
    color: '#666',
    marginTop: 5,
  },
  interactiveBox: {
    width: '90%',
    padding: 20,
    backgroundColor: '#e3f2fd',
    borderRadius: 10,
    alignItems: 'center',
    marginBottom: 20,
  },
  counterText: {
    fontSize: 18,
    marginBottom: 10,
    fontWeight: '600',
  },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    width: '90%',
    justifyContent: 'space-between',
    paddingHorizontal: 20,
    marginBottom: 10,
  },
  hiddenBox: {
    backgroundColor: '#4caf50',
    padding: 15,
    borderRadius: 8,
    marginBottom: 20,
  },
  input: {
    width: '90%',
    height: 50,
    borderColor: '#ccc',
    borderWidth: 1,
    borderRadius: 8,
    paddingHorizontal: 15,
    backgroundColor: '#fff',
    marginTop: 10,
  },
});
