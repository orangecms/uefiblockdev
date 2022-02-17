/* UEFI TPM interface.
 *
 * This is a simplified TPM interface using the UEFI TCG protocols
 * and the "well known" software interrupt interface.
 */
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/tpm.h>
#include "efiwrapper.h"
#include "Tcg2Protocol.h"

// why is this not available?
extern struct tpm_chip *tpmm_chip_alloc(struct device *pdev,
                                 const struct tpm_class_ops *ops);
extern int tpm_chip_register(struct tpm_chip *chip);

// where is this defined?
typedef struct __attribute__((__packed__)) {
  uint16_t    tag;
  uint32_t    paramSize;
  uint32_t    responseCode;
} TPM2_RESPONSE_HEADER;


typedef struct {
	spinlock_t lock;
	EFI_TCG2_PROTOCOL * uefi_tpm;
	struct tpm_chip * chip;
	uint8_t recv_buf[4096];
} uefi_tpm_t;


static struct platform_device * pdev;

static int tpm_response_len(const uint8_t * buf)
{
        /* size of the data received in the TPM2_RESPONSE_HEADER struct */
	__be32 *native_size = (__force __be32 *) &buf[2];
        return be32_to_cpu(*native_size);
}


static int uefi_tpm_send(struct tpm_chip * chip, u8 *buf, size_t len)
{
	uefi_tpm_t * const priv = dev_get_drvdata(&chip->dev);
	unsigned long flags;
	int status;
	//printk("uefi tpm send %zu\n", len);

	uefi_memory_map_add();

	spin_lock_irqsave(&priv->lock, flags);
	memset(priv->recv_buf, 0xCC, sizeof(priv->recv_buf));

	status = priv->uefi_tpm->SubmitCommand(
		priv->uefi_tpm,
		len,
		buf,
		sizeof(priv->recv_buf),
		priv->recv_buf
	);

	//print_hex_dump(KERN_INFO, "recv data", DUMP_PREFIX_OFFSET, 16, 1, priv->recv_buf, tpm_response_len(priv->recv_buf), true);

	spin_unlock_irqrestore(&priv->lock, flags);

	if (status != 0)
	{
		printk("uefi tpm error: %d\n", status);
		return -1;
	}


	//printk("uefi tpm send rc: %d\n", status);

	return status;
}

static int uefi_tpm_recv(struct tpm_chip * chip, u8 *buf, size_t len)
{
	uefi_tpm_t * const priv = dev_get_drvdata(&chip->dev);
	unsigned long flags;
	uint32_t size;

	spin_lock_irqsave(&priv->lock, flags);

        size = tpm_response_len(priv->recv_buf);
	//printk("uefi tpm recv %u\n", size);

	if (size > len)
	{
		spin_unlock_irqrestore(&priv->lock, flags);
		return -EIO;
	}

	memcpy(buf, priv->recv_buf, size);
	spin_unlock_irqrestore(&priv->lock, flags);

	return size;
}

static u8 uefi_tpm_status(struct tpm_chip * chip)
{
	return 0; // always ready
}

static int uefi_tpm_not_implemented(void)
{
	printk("uefi tpm function not implemented\n");
	return -1;
}

static const struct tpm_class_ops uefi_tpm_ops = {
	.flags		= 0,
	.send		= uefi_tpm_send,
	.recv		= uefi_tpm_recv,
	.status		= uefi_tpm_status,
	.cancel		= (void*) uefi_tpm_not_implemented,
	.req_canceled	= (void*) uefi_tpm_not_implemented,
};

int uefi_tpm_init(void)
{
	uefi_tpm_t * priv;
	EFI_TCG2_BOOT_SERVICE_CAPABILITY caps = { .Size = sizeof(caps) };

	pdev = platform_device_register_simple("tpm_uefi", -1, NULL, 0);
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	priv->uefi_tpm = uefi_locate_and_handle_protocol(&EFI_TCG2_PROTOCOL_GUID);

	if (!priv->uefi_tpm)
	{
		printk("uefi tpm: not present?\n");
		platform_device_unregister(pdev);
		return 0;
	}

	priv->uefi_tpm->GetCapability(priv->uefi_tpm, &caps);
	printk("uefi tpm: present=%d manufacturer=%08x\n",
		caps.TPMPresentFlag,
		caps.ManufacturerID
	);

	spin_lock_init(&priv->lock);
	priv->chip = tpmm_chip_alloc(&pdev->dev, &uefi_tpm_ops);
	dev_set_drvdata(&priv->chip->dev, priv);

	tpm_chip_register(priv->chip);

	return 0;
}
