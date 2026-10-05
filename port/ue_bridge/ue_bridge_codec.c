/*
UE_BRIDGE_CODEC.C

See ue_bridge_codec.h.
*/

#include "ue_bridge_codec.h"

void ue_bridge_unpack_normal(uint32_t packed, float out[3])
{
	/* each field shifted to the top of a signed 32-bit value, then scaled:
	uncompress_int32_to_real_vector3d's arithmetic, step for step */
	float value = (float)(int32_t)(packed << 21);

	out[0] = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	packed >>= 11;
	value = (float)(int32_t)(packed << 21);
	out[1] = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	packed >>= 11;
	value = (float)(int32_t)(packed << 22);
	out[2] = (value * (1.0f / 2097152.0f) + 1.0f) * (1.0f / 1023.0f);
}

void ue_bridge_decode_model_vertex(const struct ue_bridge_compressed_model_vertex *in, struct ue_bridge_model_vertex *out)
{
	out->position[0] = in->position[0];
	out->position[1] = in->position[1];
	out->position[2] = in->position[2];
	ue_bridge_unpack_normal(in->normal, out->normal);
	/* NORMSHORT2 */
	out->uv[0] = (float)in->texcoord[0] / 32767.0f;
	out->uv[1] = (float)in->texcoord[1] / 32767.0f;
	out->node[0] = (uint8_t)(in->nodes[0] / 3u);
	out->node[1] = (uint8_t)(in->nodes[1] / 3u);
	out->reserved = 0;
	/* NORMSHORT1, as the GPU reads it; the decomp's CPU decoder takes the low
	byte over 255 instead, which is not what the Xbox draws */
	out->weight = (float)in->node_weight / 32767.0f;
}

void ue_bridge_decode_environment_vertex(const struct ue_bridge_compressed_environment_vertex *in, struct ue_bridge_bsp_vertex *out)
{
	out->position[0] = in->position[0];
	out->position[1] = in->position[1];
	out->position[2] = in->position[2];
	ue_bridge_unpack_normal(in->normal, out->normal);
	out->uv[0] = in->texcoord[0];
	out->uv[1] = in->texcoord[1];
}

uint32_t ue_bridge_strip_to_list(const uint16_t *strip, uint32_t strip_length, ue_bridge_model_index *list)
{
	uint32_t written = 0;
	uint32_t index;

	for (index = 0; index + 2u < strip_length; index++)
	{
		uint16_t a = strip[index], b = strip[index + 1u], c = strip[index + 2u];

		if (a == b || b == c || a == c)
			continue;
		/* a strip flips facing every triangle: swapping an odd triangle's
		first two keeps them all facing the same way */
		if (index & 1u)
		{
			list[written++] = b;
			list[written++] = a;
		}
		else
		{
			list[written++] = a;
			list[written++] = b;
		}
		list[written++] = c;
	}
	return written;
}
