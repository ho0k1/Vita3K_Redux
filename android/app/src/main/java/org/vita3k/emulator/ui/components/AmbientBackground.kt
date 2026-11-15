package org.vita3k.emulator.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.sin
import kotlin.random.Random

private val AmbientTop = Color(0xFF241F25)
private val AmbientMid = Color(0xFF16151C)
private val AmbientBottom = Color(0xFF0B0B12)
private val BokehGold = Color(0xFFFFD79A)

private class Bokeh(
    val x: Float,
    val y: Float,
    val radius: Float,
    val alpha: Float,
    val phase: Float,
    val drift: Float
)

/**
 * Redux signature background: deep dusk gradient with static golden bokeh.
 * Draws only; place content on top of it.
 */
@Composable
fun AmbientBackground(modifier: Modifier = Modifier, particles: Int = 46) {
    val bokeh = remember(particles) {
        val rnd = Random(2026)
        List(particles) {
            // denser band across the lower-middle of the screen
            val band = rnd.nextFloat()
            Bokeh(
                x = rnd.nextFloat(),
                y = if (band < 0.7f) 0.45f + rnd.nextFloat() * 0.5f else rnd.nextFloat(),
                radius = if (rnd.nextFloat() < 0.18f) 9f + rnd.nextFloat() * 14f else 1.5f + rnd.nextFloat() * 5f,
                alpha = 0.18f + rnd.nextFloat() * 0.5f,
                phase = rnd.nextFloat(),
                drift = 0.004f + rnd.nextFloat() * 0.012f
            )
        }
    }
    Canvas(
        modifier = modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(AmbientTop, AmbientMid, AmbientBottom)))
    ) {
        val density = size.minDimension / 360f
        bokeh.forEach { b ->
            val cx = b.x * size.width
            val cy = b.y * size.height
            val twinkle = 0.65f + 0.35f * sin(b.phase * 6.2832f)
            val r = b.radius * density
            val a = b.alpha * twinkle
            drawCircle(
                brush = Brush.radialGradient(
                    colors = listOf(BokehGold.copy(alpha = a), BokehGold.copy(alpha = a * 0.35f), Color.Transparent),
                    center = Offset(cx, cy),
                    radius = r * 1.6f
                ),
                radius = r * 1.6f,
                center = Offset(cx, cy)
            )
        }
    }
}

/** White disc with the dark "R" mark, the Redux emblem. */
@Composable
fun ReduxEmblem(size: Dp, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .size(size)
            .clip(CircleShape)
            .background(Color.White),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = "R",
            color = Color(0xFF14131A),
            fontWeight = FontWeight.Black,
            fontSize = (size.value * 0.58f).sp
        )
    }
}
