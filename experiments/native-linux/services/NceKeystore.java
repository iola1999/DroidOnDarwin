/* SPDX-License-Identifier: GPL-2.0-or-later */
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public final class NceKeystore {
    public static void main(String[] args) throws Exception {
        final String alias = "nce-desktop-regression-v1";
        final byte[] message = "NCE persistent Android Keystore".getBytes(StandardCharsets.UTF_8);
        File record = new File("/data/system/nce-keystore-check");
        // Standalone app_process does not run ActivityThread provider setup.
        Class.forName("android.security.keystore2.AndroidKeyStoreProvider").getMethod("install").invoke(null);
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        boolean restored = record.isFile();
        if (!restored) {
            KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(alias,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
            SecretKey key = generator.generateKey();
            Cipher encrypt = Cipher.getInstance("AES/GCM/NoPadding");
            encrypt.init(Cipher.ENCRYPT_MODE, key);
            byte[] encrypted = encrypt.doFinal(message);
            try (FileOutputStream file = new FileOutputStream(record);
                 DataOutputStream output = new DataOutputStream(file)) {
                output.writeInt(encrypt.getIV().length);
                output.write(encrypt.getIV());
                output.writeInt(encrypted.length);
                output.write(encrypted);
                output.flush();
                file.getFD().sync();
            }
        }
        SecretKey key = (SecretKey) store.getKey(alias, null);
        if (key == null || key.getEncoded() != null)
            throw new AssertionError("persistent Keystore key missing or exportable");
        byte[] iv, encrypted;
        try (DataInputStream input = new DataInputStream(new FileInputStream(record))) {
            int size = input.readInt();
            if (size != 12) throw new AssertionError("invalid GCM IV length");
            iv = new byte[size];
            input.readFully(iv);
            size = input.readInt();
            if (size != message.length + 16) throw new AssertionError("invalid ciphertext length");
            encrypted = new byte[size];
            input.readFully(encrypted);
            if (input.read() != -1) throw new AssertionError("unexpected ciphertext trailing data");
        }
        Cipher decrypt = Cipher.getInstance("AES/GCM/NoPadding");
        decrypt.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(128, iv));
        if (!Arrays.equals(message, decrypt.doFinal(encrypted)))
            throw new AssertionError("Keystore plaintext mismatch");
        System.out.println("{\"test\":\"android_keystore_persistence\",\"passed\":true,\"restored\":" + restored + "}");
    }
}
