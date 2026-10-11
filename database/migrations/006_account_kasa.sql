-- Kasa Erişimi (guide l.875-878: "Karakterinizde Kasa Erişimi ürününüz varsa, kişisel bilgi penceresine sağ tuşla tıklayarak
-- Kasamı Göster seçeneğiyle herhangi bir yerde kasanıza erişebilirsiniz"). The original came from the web İKV Dükkan and was
-- loaded onto a character; here an administrator grants it per account (POST /v1/admin/accounts/{id}/kasa|unkasa). NONCANONICAL source.
ALTER TABLE accounts ADD COLUMN kasa boolean NOT NULL DEFAULT false;
