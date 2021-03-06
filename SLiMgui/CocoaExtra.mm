//
//  CocoaExtra.m
//  SLiM
//
//  Created by Ben Haller on 1/22/15.
//  Copyright (c) 2015-2017 Philipp Messer.  All rights reserved.
//	A product of the Messer Lab, http://messerlab.org/slim/
//

//	This file is part of SLiM.
//
//	SLiM is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
//
//	SLiM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License along with SLiM.  If not, see <http://www.gnu.org/licenses/>.


#import "CocoaExtra.h"
#import "AppDelegate.h"

#include "eidos_rng.h"
#import "mutation_type.h"


@implementation SLiMTableView

- (BOOL)acceptsFirstResponder
{
	return NO;
}

@end


static NSDictionary *tickAttrs = nil;
static NSDictionary *disabledTickAttrs = nil;
static const int numberOfTicks = 4;
static const int tickLength = 4;
static const int heightForTicks = 15;

@implementation SLiMColorStripeView

+ (void)initialize
{
	if (!tickAttrs)
		tickAttrs = [@{NSForegroundColorAttributeName : [NSColor blackColor], NSFontAttributeName : [NSFont systemFontOfSize:9.0]} retain];
	if (!disabledTickAttrs)
		disabledTickAttrs = [@{NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.6 alpha:1.0], NSFontAttributeName : [NSFont systemFontOfSize:9.0]} retain];
	
	[self exposeBinding:@"enabled"];
}

- (void)setEnabled:(BOOL)enabled
{
	if (_enabled != enabled)
	{
		_enabled = enabled;
		
		[self setNeedsDisplay:YES];
	}
}

- (void)awakeFromNib
{
	[self bind:@"enabled" toObject:[[self window] windowController] withKeyPath:@"invalidSimulation" options:@{NSValueTransformerNameBindingOption : NSNegateBooleanTransformerName}];
}

- (void)dealloc
{
	[self unbind:@"enabled"];
	
	[super dealloc];
}

- (BOOL)isOpaque
{
	return NO;	// we have a 10-pixel margin on our left and right to allow tick labels to overflow our apparent bounds
}

- (void)setMetricToPlot:(int)metricToPlot
{
	if (_metricToPlot != metricToPlot)
	{
		_metricToPlot = metricToPlot;
		
		[self setNeedsDisplay:YES];
	}
}

- (void)drawTicksInContentRect:(NSRect)contentRect
{
	BOOL enabled = [self enabled];
	int metric = [self metricToPlot];
	NSRect interiorRect = NSInsetRect(contentRect, 1, 1);
	
	for (int tickIndex = 0; tickIndex <= numberOfTicks; ++tickIndex)
	{
		double fraction = tickIndex / (double)numberOfTicks;
		int tickLeft = (int)floor(interiorRect.origin.x + fraction * (interiorRect.size.width - 1));
		NSRect tickRect = NSMakeRect(tickLeft, contentRect.origin.y - tickLength, 1, tickLength);
		
		[[NSColor colorWithCalibratedWhite:(enabled ? 0.5 : 0.6) alpha:1.0] set];
		NSRectFill(tickRect);
		
		NSString *tickLabel = nil;
		
		if (metric == 1)
		{
			if (tickIndex == 0) tickLabel = @"0.0";
			if (tickIndex == 1) tickLabel = @"0.5";
			if (tickIndex == 2) tickLabel = @"1.0";
			if (tickIndex == 3) tickLabel = @"2.0";
			if (tickIndex == 4) tickLabel = @"∞";
		}
		else if (metric == 2)
		{
			if (tickIndex == 0) tickLabel = @"−0.5";
			if (tickIndex == 1) tickLabel = @"−0.25";
			if (tickIndex == 2) tickLabel = @"0.0";
			if (tickIndex == 3) tickLabel = @"0.5";
			if (tickIndex == 4) tickLabel = @"1.0";
		}
		
		if (tickLabel)
		{
			NSAttributedString *tickAttrLabel = [[NSAttributedString alloc] initWithString:tickLabel attributes:(enabled ? tickAttrs : disabledTickAttrs)];
			NSSize tickLabelSize = [tickAttrLabel size];
			int tickLabelX = tickLeft - (int)round(tickLabelSize.width / 2.0);
			
			[tickAttrLabel drawAtPoint:NSMakePoint(tickLabelX, contentRect.origin.y - (tickLength + 12))];
			
			[tickAttrLabel release];
		}
	}
}

- (void)drawRect:(NSRect)dirtyRect
{
	NSRect bounds = [self bounds];
	NSRect contentRect = NSMakeRect(bounds.origin.x + 10, bounds.origin.y + heightForTicks, bounds.size.width - 20, bounds.size.height - heightForTicks);
	NSRect interiorRect = NSInsetRect(contentRect, 1, 1);
	
	// frame the content area itself
	[[NSColor colorWithCalibratedWhite:0.6 alpha:1.0] set];
	NSFrameRect(contentRect);
	
	double scaling = [self scalingFactor];
	int metric = [self metricToPlot];
	
	// draw our stripe
	for (int x = 0; x < interiorRect.size.width; ++x)
	{
		NSRect stripe = NSMakeRect(interiorRect.origin.x + x, interiorRect.origin.y, 1, interiorRect.size.height);
		float red = 0.0, green = 0.0, blue = 0.0;
		double fraction = x / (interiorRect.size.width - 1);
		
		// guarantee that there is a pixel position where fraction is 0.5, so neutrality gets drawn
		if (x == floor((interiorRect.size.width - 1) / 2.0))
			fraction = 0.5;
		
		if (metric == 1)
		{
			double fitness;
			
			if (fraction < 0.5) fitness = fraction * 2.0;						// [0.0, 0.5] -> [0.0, 1.0]
			else if (fraction < 0.75) fitness = (fraction - 0.5) * 4.0 + 1.0;	// [0.5, 0.75] -> [1.0, 2.0]
			else fitness = 0.50 / (0.25 - (fraction - 0.75));					// [0.75, 1.0] -> [2.0, +Inf]
			
			if (fraction == 1.0)
				fitness = 1e100;	// avoid infinity
			
			RGBForFitness(fitness, &red, &green, &blue, scaling);
		}
		else if (metric == 2)
		{
			double selectionCoeff;
			
			if (fraction < 0.5) selectionCoeff = fraction - 0.5;				// [0.0, 0.5] -> [-0.5, 0]
			else selectionCoeff = (fraction - 0.5) * 2.0;						// [0.5, 1.0] -> [0.0, 1.0]
			
			RGBForSelectionCoeff(selectionCoeff, &red, &green, &blue, scaling);
		}
		
		[[NSColor colorWithCalibratedRed:red green:green blue:blue alpha:1.0] set];
		NSRectFill(stripe);
	}
	
	// draw ticks at bottom of content rect
	[self drawTicksInContentRect:contentRect];
	
	// if we are not enabled, wash over the interior with light gray
	if (![self enabled])
	{
		[[NSColor colorWithCalibratedWhite:0.9 alpha:0.8] set];
		NSRectFillUsingOperation(interiorRect, NSCompositeSourceOver);
	}
}

@end

const float greenBrightness = 0.8f;

void RGBForFitness(double value, float *colorRed, float *colorGreen, float *colorBlue, double scalingFactor)
{
	// apply the scaling factor
	value = (value - 1.0) * scalingFactor + 1.0;
	
	if (value <= 0.5)
	{
		// value <= 0.5 is a shade of red, going down to black
		*colorRed = (float)(value * 2.0);
		*colorGreen = 0.0;
		*colorBlue = 0.0;
	}
	else if (value >= 2.0)
	{
		// value >= 2.0 is a shade of green, going up to white
		*colorRed = (float)((value - 2.0) * greenBrightness / value);
		*colorGreen = greenBrightness;
		*colorBlue = (float)((value - 2.0) * greenBrightness / value);
	}
	else if (value <= 1.0)
	{
		// value <= 1.0 (but > 0.5) goes from red (unfit) to yellow (neutral)
		*colorRed = 1.0;
		*colorGreen = (float)((value - 0.5) * 2.0);
		*colorBlue = 0.0;
	}
	else	// 1.0 < value < 2.0
	{
		// value > 1.0 (but < 2.0) goes from yellow (neutral) to green (fit)
		*colorRed = (float)(2.0 - value);
		*colorGreen = (float)(greenBrightness + (1.0 - greenBrightness) * (2.0 - value));
		*colorBlue = 0.0;
	}
}

void RGBForSelectionCoeff(double value, float *colorRed, float *colorGreen, float *colorBlue, double scalingFactor)
{
	// apply a scaling factor; this could be user-adjustible since different models have different relevant fitness ranges
	value *= scalingFactor;
	
	// and add 1, just so we can re-use the same code as in RGBForFitness()
	value += 1.0;
	
	if (value <= 0.0)
	{
		// value <= 0.0 is the darkest shade of red we use
		*colorRed = 0.5;
		*colorGreen = 0.0;
		*colorBlue = 0.0;
	}
	else if (value <= 0.5)
	{
		// value <= 0.5 is a shade of red, going down toward black
		*colorRed = (float)(value + 0.5);
		*colorGreen = 0.0;
		*colorBlue = 0.0;
	}
	else if (value < 1.0)
	{
		// value <= 1.0 (but > 0.5) goes from red (very unfit) to orange (nearly neutral)
		*colorRed = 1.0;
		*colorGreen = (float)((value - 0.5) * 1.0);
		*colorBlue = 0.0;
	}
	else if (value == 1.0)
	{
		// exactly neutral mutations are yellow
		*colorRed = 1.0;
		*colorGreen = 1.0;
		*colorBlue = 0.0;
	}
	else if (value <= 1.5)
	{
		// value > 1.0 (but < 1.5) goes from green (nearly neutral) to cyan (fit)
		*colorRed = 0.0;
		*colorGreen = greenBrightness;
		*colorBlue = (float)((value - 1.0) * 2.0);
	}
	else if (value <= 2.0)
	{
		// value > 1.5 (but < 2.0) goes from cyan (fit) to blue (very fit)
		*colorRed = 0.0;
		*colorGreen = (float)(greenBrightness * ((2.0 - value) * 2.0));
		*colorBlue = 1.0;
	}
	else // (value > 2.0)
	{
		// value > 2.0 is a shade of blue, going up toward white
		*colorRed = (float)((value - 2.0) * 0.75 / value);
		*colorGreen = (float)((value - 2.0) * 0.75 / value);
		*colorBlue = 1.0;
	}
}


@implementation SLiMMenuButton

- (void)dealloc
{
	[self setSlimMenu:nil];
	
	[super dealloc];
}

- (void)fixMenu
{
	NSMenu *menu = [self slimMenu];
	NSDictionary *itemAttrs = @{NSFontAttributeName : [NSFont systemFontOfSize:12.0]};
	
	for (int i = 0; i < [menu numberOfItems]; ++i)
	{
		NSMenuItem *menuItem = [menu itemAtIndex:i];
		NSString *title = [menuItem title];
		NSAttributedString *attrTitle = [[NSAttributedString alloc] initWithString:title attributes:itemAttrs];
		
		[menuItem setAttributedTitle:attrTitle];
		[attrTitle release];
	}
}

- (void)mouseDown:(NSEvent *)theEvent
{
	// We do not call super; we do mouse tracking entirely ourselves
	//[super mouseDown:theEvent];
	
	if ([self isEnabled])
	{
		NSRect bounds = [self bounds];
		
		[self highlight:YES];
		
		[self fixMenu];
		[[self slimMenu] popUpMenuPositioningItem:nil atLocation:NSMakePoint(bounds.size.width * 0.80 - 1, bounds.size.height * 0.80 + 1) inView:self];
		
		[self highlight:NO];
	}
}

- (void)mouseDragged:(NSEvent *)theEvent
{
	// We do not call super; we do mouse tracking entirely ourselves
	//[super mouseDragged:theEvent];
}

- (void)mouseUp:(NSEvent *)theEvent
{
	// We do not call super; we do mouse tracking entirely ourselves
	//[super mouseDown:theEvent];
}

@end

@implementation SLiMColorCell

- (void)setObjectValue:(id)objectValue
{
	// Ensure that only NSColor objects are set as our object value
	if ([objectValue isKindOfClass:[NSColor class]])
		[super setObjectValue:objectValue];
}

- (void)drawWithFrame:(NSRect)cellFrame inView:(NSView *)controlView
{
	NSRect swatchFrame = NSInsetRect(cellFrame, 1, 1);
	
	// Make our swatch be square, if we have enough room to do it
	if (swatchFrame.size.width > swatchFrame.size.height)
	{
		swatchFrame.origin.x = (int)round(swatchFrame.origin.x + (swatchFrame.size.width - swatchFrame.size.height) / 2.0);
		swatchFrame.size.width = swatchFrame.size.height;
	}
	
	[[NSColor blackColor] set];
	NSFrameRect(swatchFrame);
	
	[(NSColor *)[self objectValue] set];
	NSRectFill(NSInsetRect(swatchFrame, 1, 1));
}

@end


@implementation SLiMSelectionView

- (void)drawRect:(NSRect)dirtyRect
{
	static NSDictionary *labelAttrs = nil;
	
	if (!labelAttrs)
		labelAttrs = [@{NSFontAttributeName : [NSFont fontWithName:@"Times New Roman" size:10], NSForegroundColorAttributeName : [NSColor blackColor]} retain];
	
	NSRect bounds = [self bounds];
	SLiMSelectionMarker *marker = (SLiMSelectionMarker *)[self window];
	NSAttributedString *attrLabel = [[NSAttributedString alloc] initWithString:[marker label] attributes:labelAttrs];
	NSSize labelStringSize = [attrLabel size];
	NSSize labelSize = NSMakeSize(round(labelStringSize.width + 8.0), round(labelStringSize.height + 1.0));
	NSRect labelRect = NSMakeRect(bounds.origin.x + round(bounds.size.width / 2.0), bounds.origin.y + bounds.size.height - labelSize.height, labelSize.width, labelSize.height);
	
	if ([marker isLeftMarker])
		labelRect.origin.x -= round(labelSize.width - 1.0);
	
	// Frame our whole bounds, for debugging; note that we draw in only a portion of our bounds, and the rest is transparent
	//[[NSColor blackColor] set];
	//NSFrameRect(bounds);
	
#if 0
	// Debugging code: frame and fill our label rect without using NSBezierPath
	[[NSColor colorWithCalibratedHue:0.15 saturation:0.2 brightness:1.0 alpha:1.0] set];
	NSRectFill(labelRect);
	
	[[NSColor colorWithCalibratedHue:0.15 saturation:0.2 brightness:0.3 alpha:1.0] set];
	NSFrameRect(labelRect);
#else
	// Production code: use NSBezierPath to get a label that has a tag off of it
	NSBezierPath *bez = [NSBezierPath bezierPath];
	NSRect ilr = NSInsetRect(labelRect, 0.5, 0.5);	// inset by 0.5 to place us mid-pixel, so stroke looks good
	const double tagHeight = 5.0;
	
	if ([marker isLeftMarker])
	{
		// label rect with a diagonal tag down from the right edge
		[bez moveToPoint:NSMakePoint(ilr.origin.x, ilr.origin.y)];
		[bez relativeLineToPoint:NSMakePoint(ilr.size.width - tagHeight, 0)];
		[bez relativeLineToPoint:NSMakePoint(tagHeight, -tagHeight)];
		[bez relativeLineToPoint:NSMakePoint(0, ilr.size.height + tagHeight)];
		[bez relativeLineToPoint:NSMakePoint(-ilr.size.width, 0)];
		[bez closePath];
	}
	else
	{
		// label rect with a diagonal tag down from the right edge
		[bez moveToPoint:NSMakePoint(ilr.origin.x + ilr.size.width, ilr.origin.y)];
		[bez relativeLineToPoint:NSMakePoint(- (ilr.size.width - tagHeight), 0)];
		[bez relativeLineToPoint:NSMakePoint(-tagHeight, -tagHeight)];
		[bez relativeLineToPoint:NSMakePoint(0, ilr.size.height + tagHeight)];
		[bez relativeLineToPoint:NSMakePoint(ilr.size.width, 0)];
		[bez closePath];
	}
	
	[[NSColor colorWithCalibratedHue:0.15 saturation:0.2 brightness:1.0 alpha:1.0] set];
	[bez fill];
	
	[[NSColor blackColor] set];
	[bez setLineWidth:1.0];
	[bez stroke];
#endif
	
	[attrLabel drawAtPoint:NSMakePoint(labelRect.origin.x + 4, labelRect.origin.y + 1)];
	[attrLabel release];
}

- (BOOL)isOpaque
{
	return NO;
}

@end

@implementation SLiMSelectionMarker

// makes a new marker with no label and no tip point, not shown
+ (instancetype)new
{
	return [[[self class] alloc] initWithContentRect:NSMakeRect(0, 0, 150, 20) styleMask:NSBorderlessWindowMask backing:NSBackingStoreBuffered defer:YES];	// 150x20 should suffice, unless we change our font size...
}

- (instancetype)initWithContentRect:(NSRect)contentRect styleMask:(NSUInteger)aStyle backing:(NSBackingStoreType)bufferingType defer:(BOOL)flag
{
	if (self = [super initWithContentRect:contentRect styleMask:aStyle backing:bufferingType defer:flag])
	{
		[self setFloatingPanel:YES];
		[self setBecomesKeyOnlyIfNeeded:YES];
		[self setHasShadow:NO];
		[self setOpaque:NO];
		[self setBackgroundColor:[NSColor clearColor]];
		
		SLiMSelectionView *view = [[SLiMSelectionView alloc] initWithFrame:contentRect];
		
		[self setContentView:view];
		[view release];
		
		_tipPoint = NSMakePoint(round(contentRect.origin.x + contentRect.size.width / 2.0), contentRect.origin.y);
		_label = [@"1000000000" retain];
	}
	
	return self;
}
	
- (void)dealloc
{
	[self setLabel:nil];
	
	[super dealloc];
}

- (void)setLabel:(NSString *)label
{
	if (![_label isEqualToString:label])
	{
		[label retain];
		[_label release];
		_label = label;
		
		[[self contentView] setNeedsDisplay:YES];
	}
}

- (void)setTipPoint:(NSPoint)tipPoint
{
	if (!NSEqualPoints(_tipPoint, tipPoint))
	{
		NSPoint origin = [self frame].origin;
		
		origin.x += (tipPoint.x - _tipPoint.x);
		origin.y += (tipPoint.y - _tipPoint.y);
		
		_tipPoint = tipPoint;
		
		[self setFrameOrigin:origin];
	}
}

@end


@implementation SLiMPlaySliderToolTipView

- (void)drawRect:(NSRect)dirtyRect
{
	static NSDictionary *labelAttrs = nil;
	
	if (!labelAttrs)
		labelAttrs = [@{NSFontAttributeName : [NSFont fontWithName:@"Times New Roman" size:10], NSForegroundColorAttributeName : [NSColor blackColor]} retain];
	
	NSRect bounds = [self bounds];
	SLiMPlaySliderToolTipWindow *tooltipWindow = (SLiMPlaySliderToolTipWindow *)[self window];
	NSAttributedString *attrLabel = [[NSAttributedString alloc] initWithString:[tooltipWindow label] attributes:labelAttrs];
	NSSize labelStringSize = [attrLabel size];
	NSSize labelSize = NSMakeSize(round(labelStringSize.width + 8.0), round(labelStringSize.height + 1.0));
	NSRect labelRect = NSMakeRect(bounds.origin.x, bounds.origin.y + bounds.size.height - labelSize.height, labelSize.width, labelSize.height);
	
	// Frame our whole bounds, for debugging; note that we draw in only a portion of our bounds, and the rest is transparent
	//[[NSColor blackColor] set];
	//NSFrameRect(bounds);
	
	// Frame and fill our label rect
	[[NSColor colorWithCalibratedHue:0.15 saturation:0.2 brightness:1.0 alpha:1.0] set];
	NSRectFill(labelRect);
	
	[[NSColor colorWithCalibratedHue:0.15 saturation:0.2 brightness:0.3 alpha:1.0] set];
	NSFrameRect(labelRect);
	
	[attrLabel drawAtPoint:NSMakePoint(labelRect.origin.x + 4, labelRect.origin.y + 1)];
	[attrLabel release];
}

- (BOOL)isOpaque
{
	return NO;
}

@end

@implementation SLiMPlaySliderToolTipWindow

// makes a new marker with no label and no tip point, not shown
+ (instancetype)new
{
	return [[[self class] alloc] initWithContentRect:NSMakeRect(0, 0, 50, 20) styleMask:NSBorderlessWindowMask backing:NSBackingStoreBuffered defer:YES];	// 50x20 should suffice, unless we change our font size...
}

- (instancetype)initWithContentRect:(NSRect)contentRect styleMask:(NSUInteger)aStyle backing:(NSBackingStoreType)bufferingType defer:(BOOL)flag
{
	if (self = [super initWithContentRect:contentRect styleMask:aStyle backing:bufferingType defer:flag])
	{
		[self setFloatingPanel:YES];
		[self setBecomesKeyOnlyIfNeeded:YES];
		[self setHasShadow:NO];
		[self setOpaque:NO];
		[self setBackgroundColor:[NSColor clearColor]];
		
		SLiMPlaySliderToolTipView *view = [[SLiMPlaySliderToolTipView alloc] initWithFrame:contentRect];
		
		[self setContentView:view];
		[view release];
		
		_tipPoint = NSMakePoint(contentRect.origin.x, contentRect.origin.y);
		_label = [@"1000000000" retain];
	}
	
	return self;
}

- (void)dealloc
{
	[self setLabel:nil];
	
	[super dealloc];
}

- (void)setLabel:(NSString *)label
{
	if (![_label isEqualToString:label])
	{
		[label retain];
		[_label release];
		_label = label;
		
		[[self contentView] setNeedsDisplay:YES];
	}
}

- (void)setTipPoint:(NSPoint)tipPoint
{
	if (!NSEqualPoints(_tipPoint, tipPoint))
	{
		NSPoint origin = [self frame].origin;
		
		origin.x += (tipPoint.x - _tipPoint.x);
		origin.y += (tipPoint.y - _tipPoint.y);
		
		_tipPoint = tipPoint;
		
		[self setFrameOrigin:origin];
	}
}

@end


@implementation SLiMMutationTypeDFEToolTipView

- (void)drawRect:(NSRect)dirtyRect
{
	static NSDictionary *labelAttrs = nil;
	static NSDictionary *questionMarkAttrs = nil;
	
	if (!labelAttrs)
	{
		labelAttrs = [@{NSFontAttributeName : [NSFont fontWithName:@"Times New Roman" size:9], NSForegroundColorAttributeName : [NSColor blackColor]} retain];
		questionMarkAttrs = [@{NSFontAttributeName : [NSFont fontWithName:@"Times New Roman" size:18], NSForegroundColorAttributeName : [NSColor blackColor]} retain];
	}
	
	NSRect bounds = [self bounds];
	SLiMMutationTypeDFEToolTipWindow *tooltipWindow = (SLiMMutationTypeDFEToolTipWindow *)[self window];
	
	// Frame and fill our label rect
	[[NSColor colorWithCalibratedHue:0.0 saturation:0.0 brightness:0.95 alpha:1.0] set];
	NSRectFill(bounds);
	
	[[NSColor colorWithCalibratedHue:0.15 saturation:0.0 brightness:0.75 alpha:1.0] set];
	NSFrameRect(bounds);
	
	NSRect graphRect = NSMakeRect(bounds.origin.x + 6, bounds.origin.x + 14, bounds.size.width - 12, bounds.size.height - 20);
	
	[[NSColor colorWithCalibratedHue:0.15 saturation:0.0 brightness:0.2 alpha:1.0] set];
	NSRectFill(NSMakeRect(graphRect.origin.x, graphRect.origin.y, graphRect.size.width, 1));
	
	NSRectFill(NSMakeRect(graphRect.origin.x, graphRect.origin.y - 3, 1, 3));
	NSRectFill(NSMakeRect(graphRect.origin.x + (graphRect.size.width - 1) * 0.125, graphRect.origin.y - 1, 1, 1));
	NSRectFill(NSMakeRect(graphRect.origin.x + (graphRect.size.width - 1) * 0.25, graphRect.origin.y - 1, 1, 1));
	NSRectFill(NSMakeRect(graphRect.origin.x + (graphRect.size.width - 1) * 0.375, graphRect.origin.y - 1, 1, 1));
	NSRectFill(NSMakeRect(graphRect.origin.x + (graphRect.size.width - 1) * 0.5, graphRect.origin.y - 3, 1, 3));
	NSRectFill(NSMakeRect(graphRect.origin.x + (graphRect.size.width - 1) * 0.625, graphRect.origin.y - 1, 1, 1));
	NSRectFill(NSMakeRect(graphRect.origin.x + (graphRect.size.width - 1) * 0.75, graphRect.origin.y - 1, 1, 1));
	NSRectFill(NSMakeRect(graphRect.origin.x + (graphRect.size.width - 1) * 0.875, graphRect.origin.y - 1, 1, 1));
	NSRectFill(NSMakeRect(graphRect.origin.x + graphRect.size.width - 1, graphRect.origin.y - 3, 1, 3));
	
	// Draw all the values we will plot; we need our own private RNG so we don't screw up the simulation's.
	// Drawing selection coefficients could raise, if they are type "s" and there is an error in the script,
	// so we run the sampling inside a try/catch block; if we get a raise, we just show a "?" in the plot.
	MutationType *mut_type = [tooltipWindow mutType];
	
	if (!mut_type)
		return;
	
	gsl_rng *save_rng = gEidos_rng;				// should be nullptr anyway, but being safe...
	static gsl_rng *local_rng = nullptr;
	std::vector<double> draws;
	bool draw_positive = false, draw_negative = false;
	const int sample_size = (mut_type->dfe_type_ == DFEType::kScript) ? 100000 : 1000000;	// large enough to make curves pretty smooth, small enough to be reasonably fast
	
	draws.reserve(sample_size);
	
	if (!local_rng)
		local_rng = gsl_rng_alloc(gsl_rng_taus2);
	
	gEidos_rng = local_rng;
	gsl_rng_set(local_rng, 10);		// arbitrary seed, but the same seed every time
	//clock_t start = clock();
	
	try
	{
		for (int sample_count = 0; sample_count < sample_size; ++sample_count)
		{
			double draw = mut_type->DrawSelectionCoefficient();
			
			draws.push_back(draw);
			
			if (draw < 0.0)			draw_negative = true;
			else if (draw > 0.0)	draw_positive = true;
		}
	}
	catch (...)
	{
		draws.clear();
		draw_negative = true;
		draw_positive = true;
	}
	
	//NSLog(@"Draws took %f seconds", (clock() - start) / (double)CLOCKS_PER_SEC);
	
	gEidos_rng = save_rng;
	
	// Decide on the axis limits and draw the axis labels
	double axis_min = -1.0, axis_max = 1.0;
	
	if (draw_negative && !draw_positive)
	{
		axis_min = -1.0;
		axis_max = 0.0;
	}
	else if (draw_positive && !draw_negative)
	{
		axis_min = 0.0;
		axis_max = 1.0;
	}
	
	NSString *axis_min_label = (axis_min == 0.0 ? @"0" : @"−1");
	NSString *axis_half_label = (axis_min == 0.0 ? @"0.5" : (axis_max == 0.0 ? @"−0.5" : @"0"));
	NSString *axis_max_label = (axis_max == 0.0 ? @"0" : @"1");
	NSSize min_label_size = [axis_min_label sizeWithAttributes:labelAttrs];
	NSSize half_label_size = [axis_half_label sizeWithAttributes:labelAttrs];
	NSSize max_label_size = [axis_max_label sizeWithAttributes:labelAttrs];
	double min_label_halfwidth = round(min_label_size.width / 2.0);
	double half_label_halfwidth = round(half_label_size.width / 2.0);
	double max_label_halfwidth = round(max_label_size.width / 2.0);
	
	[axis_min_label drawAtPoint:NSMakePoint(bounds.origin.x + 7 - min_label_halfwidth, bounds.origin.y + 1) withAttributes:labelAttrs];
	[axis_half_label drawAtPoint:NSMakePoint(bounds.origin.x + 38.5 - half_label_halfwidth, bounds.origin.y + 1) withAttributes:labelAttrs];
	[axis_max_label drawAtPoint:NSMakePoint(bounds.origin.x + 70 - max_label_halfwidth, bounds.origin.y + 1) withAttributes:labelAttrs];
	
	if (draws.size())
	{
		NSRect interiorRect = NSMakeRect(graphRect.origin.x, graphRect.origin.y + 2, graphRect.size.width, graphRect.size.height - 2);
		
		// Tabulate the distribution from the samples we took; the math here is a bit subtle, because when we are doing a -1 to +1 axis
		// we want those values to fall at bin centers, but when we're doing 0 to +1 or -1 to 0 we want 0 to fall at the bin edge.
		int half_bin_count = (int)round(interiorRect.size.width);
		int bin_count = half_bin_count * 2;								// 2x bins to look nice on Retina displays
		int32_t *bins = (int32_t *)calloc(bin_count, sizeof(int32_t));
		
		for (int sample_count = 0; sample_count < sample_size; ++sample_count)
		{
			double sel_coeff = draws[sample_count];
			int bin_index;
			
			if ((axis_min == -1.0) && (axis_max == 1.0))
				bin_index = (int)floor(((sel_coeff + 1.0) / 2.0) * (bin_count - 1) + 0.5);
			else if ((axis_min == -1.0) && (axis_max == 0.0))
				bin_index = (int)ceil((sel_coeff + 1.0) * (bin_count - 1 - 0.5) + 0.5);		// 0.0 maps to bin_count - 1, -1.0 maps to the center of bin 0
			else // if ((axis_min == 0.0) && (axis_max == 1.0))
				bin_index = (int)floor(sel_coeff * (bin_count - 1 + 0.5));					// 0.0 maps to 0, 1.0 maps to the center of bin_count - 1
			
			if ((bin_index >= 0) && (bin_index < bin_count))
				bins[bin_index]++;
		}
		
		// If we only have samples equal to zero, replicate the center column for symmetry
		if (!draw_positive && !draw_negative)
		{
			int32_t zero_count = std::max(bins[half_bin_count - 1], bins[half_bin_count]);	// whichever way it rounds...
			
			bins[half_bin_count - 1] = zero_count;
			bins[half_bin_count] = zero_count;
		}
		
		// Find the maximum bin count
		int32_t max_bin = 0;
		
		for (int bin_index = 0; bin_index < bin_count; ++bin_index)
			max_bin = std::max(max_bin, bins[bin_index]);
		
		// Plot the bins
		[[NSColor colorWithCalibratedHue:0.15 saturation:0.0 brightness:0.0 alpha:1.0] set];
		
		for (int bin_index = 0; bin_index < bin_count; ++bin_index)
		{
			if (bins[bin_index] > 0)
				NSRectFill(NSMakeRect(interiorRect.origin.x + bin_index * 0.5, interiorRect.origin.y, 0.5, interiorRect.size.height * (bins[bin_index] / (double)max_bin)));
		}
		
		free(bins);
	}
	else
	{
		// We had an exception while drawing values, so we just show a question mark
		NSString *questionMark = @"?";
		NSSize q_size = [questionMark sizeWithAttributes:questionMarkAttrs];
		double q_halfwidth = round(q_size.width / 2.0);
		
		[questionMark drawAtPoint:NSMakePoint(bounds.origin.x + bounds.size.width / 2.0 - q_halfwidth, bounds.origin.y + 22) withAttributes:questionMarkAttrs];
	}
}

- (BOOL)isOpaque
{
	return YES;
}

@end

@implementation SLiMMutationTypeDFEToolTipWindow

// makes a new marker with no label and no tip point, not shown
+ (instancetype)new
{
	return [[[self class] alloc] initWithContentRect:NSMakeRect(0, 0, 77, 50) styleMask:NSBorderlessWindowMask backing:NSBackingStoreBuffered defer:YES];
}

- (instancetype)initWithContentRect:(NSRect)contentRect styleMask:(NSUInteger)aStyle backing:(NSBackingStoreType)bufferingType defer:(BOOL)flag
{
	if (self = [super initWithContentRect:contentRect styleMask:aStyle backing:bufferingType defer:flag])
	{
		[self setFloatingPanel:YES];
		[self setBecomesKeyOnlyIfNeeded:YES];
		[self setHasShadow:NO];
		[self setOpaque:NO];
		[self setBackgroundColor:[NSColor clearColor]];
		
		SLiMMutationTypeDFEToolTipView *view = [[SLiMMutationTypeDFEToolTipView alloc] initWithFrame:contentRect];
		
		[self setContentView:view];
		[view release];
		
		_tipPoint = NSMakePoint(contentRect.origin.x, contentRect.origin.y);
		_mutType = nullptr;
	}
	
	return self;
}

- (void)setMutType:(MutationType *)mutType
{
	if (_mutType != mutType)
	{
		_mutType = mutType;
		
		[[self contentView] setNeedsDisplay:YES];
	}
}

- (void)setTipPoint:(NSPoint)tipPoint
{
	if (!NSEqualPoints(_tipPoint, tipPoint))
	{
		NSPoint origin = [self frame].origin;
		
		origin.x += (tipPoint.x - _tipPoint.x);
		origin.y += (tipPoint.y - _tipPoint.y);
		
		_tipPoint = tipPoint;
		
		[self setFrameOrigin:origin];
	}
}

@end


@implementation NSScreen (SLiMWindowFrames)

+ (BOOL)visibleCandidateWindowFrame:(NSRect)candidateFrame
{
	NSArray *screens = [NSScreen screens];
	NSUInteger nScreens = [screens count];
	
	for (NSUInteger i = 0; i < nScreens; ++i)
	{
		NSScreen *screen = [screens objectAtIndex:i];
		NSRect screenFrame = [screen visibleFrame];
		
		if (NSContainsRect(screenFrame, candidateFrame))
			return YES;
	}
	
	return NO;
}

@end


@implementation NSPopUpButton (SLiMSorting)

- (void)slimSortMenuItemsByTag
{
	NSMenu *menu = [self menu];
	int nItems = (int)[menu numberOfItems];
	
	// completely dumb bubble sort; not worth worrying about
	do
	{
		BOOL foundSwap = NO;
		
		for (int i = 0; i < nItems - 1; ++i)
		{
			NSMenuItem *firstItem = [menu itemAtIndex:i];
			NSMenuItem *secondItem = [menu itemAtIndex:i + 1];
			NSInteger firstTag = [firstItem tag];
			NSInteger secondTag = [secondItem tag];
			
			if (firstTag > secondTag)
			{
				[secondItem retain];
				[menu removeItemAtIndex:i + 1];
				[menu insertItem:secondItem atIndex:i];
				[secondItem release];
				
				foundSwap = YES;
			}
		}
		
		if (!foundSwap)
			break;
	}
	while (YES);
}

@end

@implementation NSButton (SLiMTinting)

- (void)slimSetTintColor:(NSColor *)tintColor
{
	[self setContentFilters:[NSArray array]];
	
	if (tintColor)
	{
		if (!self.layer)
			[self setWantsLayer:YES];
		
		CIFilter *tintFilter = [CIFilter filterWithName:@"CIColorMatrix"];
		
		if (tintFilter)
		{
			CGFloat redComponent = [tintColor redComponent];
			CGFloat greenComponent = [tintColor greenComponent];
			CGFloat blueComponent = [tintColor blueComponent];
			
			//NSLog(@"tintColor: redComponent == %f, greenComponent == %f, blueComponent == %f", redComponent, greenComponent, blueComponent);
			
			// The goal is to use CIColorMatrix to multiply color components so that white turns into tintColor; these vectors do that
			CIVector *rVector = [CIVector vectorWithX:redComponent Y:0.0 Z:0.0 W:0.0];
			CIVector *gVector = [CIVector vectorWithX:0.0 Y:greenComponent Z:0.0 W:0.0];
			CIVector *bVector = [CIVector vectorWithX:0.0 Y:0.0 Z:blueComponent W:0.0];
			
			[tintFilter setDefaults];
			[tintFilter setValue:rVector forKey:@"inputRVector"];
			[tintFilter setValue:gVector forKey:@"inputGVector"];
			[tintFilter setValue:bVector forKey:@"inputBVector"];
			
			[self setContentFilters:@[tintFilter]];
		}
		else
		{
			NSLog(@"could not create [CIFilter filterWithName:@\"CIColorMatrix\"]");
		}
	}
	else
	{
		// BCH added 21 May 2017: revert to non-layer-backed when tinting is removed.  This restores the previous behavior
		// of the view, which is important for making the play and profile buttons draw correctly where they overlap.  The
		// doc does not explicitly say that setWantsLayer:NO removes the layer, but in practice it does seem to.
		if (self.layer)
			[self setWantsLayer:NO];
	}
	
	[self setNeedsDisplay];
	[self.layer setNeedsDisplay];
}

@end

@implementation SLiMAutoselectTextField

- (void)mouseDown:(NSEvent *)theEvent
{
	[[self currentEditor] selectAll:nil];
}

@end


// Work around Apple's bug that they never fix that causes console logs on startup
// Thanks to TyngJJ on https://forums.developer.apple.com/thread/49052 for this workaround
// FIXME check whether they have fixed it yet, from time to time...

@interface NSWindow (FirstResponding)  
-(void)_setFirstResponder:(NSResponder *)responder;  
@end  
@interface NSDrawerWindow : NSWindow  
@end  
@implementation NSDrawerWindow (FirstResponding)  
-(void)_setFirstResponder:(NSResponder *)responder {  
	if (![responder isKindOfClass:NSView.class] || [(NSView *)responder window] == self)  
		[super _setFirstResponder:responder];  
}  
@end  

































